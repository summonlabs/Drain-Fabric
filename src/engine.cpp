#include "drain/engine.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "drain/checked.hpp"
#include "drain/digest.hpp"

namespace drain {
namespace {

/// Bounds every free-form string that can reach an explanation or a snapshot.
std::string truncate(std::string text, std::size_t limit = 256) {
  if (text.size() <= limit) {
    return text;
  }
  text.resize(limit);
  text.append("...");
  return text;
}

class LockGuard {
 public:
  LockGuard(std::mutex& mutex, std::atomic<int>& depth) : lock_(mutex), depth_(depth) {
    depth_.fetch_add(1, std::memory_order_acq_rel);
  }
  ~LockGuard() { depth_.fetch_sub(1, std::memory_order_acq_rel); }

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  std::lock_guard<std::mutex> lock_;
  std::atomic<int>& depth_;
};

bool is_fence_holding_state(DrainState state) {
  switch (state) {
    case DrainState::AdmissionClosed:
    case DrainState::Evacuating:
    case DrainState::Quiescing:
    case DrainState::Verifying:
      return true;
    default:
      return false;
  }
}

bool holds_fence(const DrainRecord& record) {
  if (is_fence_holding_state(record.state())) {
    return true;
  }
  if (record.state() == DrainState::Blocked && record.blocked_from().has_value()) {
    return is_fence_holding_state(*record.blocked_from());
  }
  return false;
}

std::string transition_action(DrainState next) { return std::string("enter-") + drain::to_string(next); }

}  // namespace

EvacuationSink::~EvacuationSink() = default;

DrainEngine::DrainEngine(Clock& clock) : DrainEngine(DrainPolicy::defaults(), clock) {}

DrainEngine::DrainEngine(DrainPolicy policy, Clock& clock)
    : clock_(clock),
      policy_(std::move(policy)),
      obligations_(policy_.max_obligations()),
      evidence_(policy_.max_evidence_records()) {}

DrainEngine::~DrainEngine() = default;

bool DrainEngine::lock_held_by_this_thread() const {
  return lock_depth_.load(std::memory_order_acquire) > 0;
}

DrainPolicy DrainEngine::policy() const {
  LockGuard guard(mutex_, lock_depth_);
  return policy_;
}

Status DrainEngine::set_policy(DrainPolicy policy, TimePoint now) {
  Status valid = policy.validate();
  if (!valid.ok()) {
    return valid;
  }
  LockGuard guard(mutex_, lock_depth_);
  policy_ = std::move(policy);
  obligations_.set_max_obligations(policy_.max_obligations());
  evidence_.set_max_records(policy_.max_evidence_records());
  Decision decision = make_decision("set-policy", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("policy=" + policy_.name());
  decision.inputs.push_back("fingerprint=" + policy_.fingerprint());
  record_decision(std::move(decision));
  return ok_status();
}

void DrainEngine::set_sink(EvacuationSink* sink) {
  LockGuard guard(mutex_, lock_depth_);
  sink_ = sink;
}

Status DrainEngine::install_incarnation(IncarnationId incarnation, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status installed = authority_.install_incarnation(incarnation, now);
  if (!installed.ok()) {
    return installed;
  }
  Decision decision = make_decision("install-incarnation", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("incarnation=" + drain::to_string(incarnation));
  record_decision(std::move(decision));
  return ok_status();
}

Status DrainEngine::begin_new_epoch(IncarnationId incarnation, TimePoint now, std::string reason) {
  const auto next_epoch = increment_checked(epoch().value());
  if (!next_epoch.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "authority epoch space exhausted");
  }
  return install_authority_line(incarnation, Epoch{*next_epoch}, now, std::move(reason));
}

Status DrainEngine::install_authority_line(IncarnationId incarnation, Epoch epoch, TimePoint now,
                                           std::string reason) {
  LockGuard guard(mutex_, lock_depth_);
  Status begun = authority_.install_line(incarnation, epoch, now, reason);
  if (!begun.ok()) {
    return begun;
  }
  // Everything recovered from a previous incarnation is untrusted: evidence must
  // be re-attested by the live incarnation before it can prove anything.
  evidence_.quarantine_all();
  Decision decision = make_decision("install-authority-line", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("incarnation=" + drain::to_string(incarnation));
  decision.inputs.push_back("epoch=" + drain::to_string(authority_.epoch()));
  decision.inputs.push_back("reason=" + truncate(reason));
  record_decision(std::move(decision));
  return ok_status();
}

IncarnationId DrainEngine::incarnation() const {
  LockGuard guard(mutex_, lock_depth_);
  return authority_.incarnation();
}

Epoch DrainEngine::epoch() const {
  LockGuard guard(mutex_, lock_depth_);
  return authority_.epoch();
}

Result<AuthorityToken> DrainEngine::issue_authority(const DomainId& scope, Duration validity, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  auto issued = authority_.issue(scope, now, validity);
  if (!issued.ok()) {
    return issued.error();
  }
  Decision decision = make_decision("issue-authority", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("authority=" + drain::to_string(issued->id()));
  decision.inputs.push_back("scope=" + (scope.empty() ? std::string("*") : scope.str()));
  record_decision(std::move(decision));
  return *issued;
}

Status DrainEngine::validate_authority(const AuthorityToken& token, TimePoint now) const {
  LockGuard guard(mutex_, lock_depth_);
  return authority_.validate(token, now);
}

Status DrainEngine::revoke_authority(AuthorityId id, std::string reason, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status revoked = authority_.revoke(id, now, reason);
  if (!revoked.ok()) {
    return revoked;
  }
  Decision decision = make_decision("revoke-authority", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("authority=" + drain::to_string(id));
  decision.inputs.push_back("reason=" + truncate(reason));
  record_decision(std::move(decision));
  return ok_status();
}

std::vector<AuthorityToken> DrainEngine::authority_tokens() const {
  LockGuard guard(mutex_, lock_depth_);
  return authority_.tokens();
}

Status DrainEngine::require_authority(AuthorityId authority, TimePoint now) const {
  if (authority.is_zero()) {
    return fail(ErrorCode::AuthorityRequired, "this operation requires an authority token");
  }
  const AuthorityToken* token = authority_.find(authority);
  if (token == nullptr) {
    return fail(ErrorCode::AuthorityRequired,
                "authority " + drain::to_string(authority) + " is not known to this incarnation");
  }
  return authority_.validate(*token, now);
}

bool DrainEngine::removal_owned_by(const DrainRecord& record) const {
  const ResourceNode* node = topology_.resource(record.target().id());
  // Unavailable is the status this runtime itself assigns during a removal
  // step. A resource that was reported Failed left service for a reason this
  // drain did not cause and must not be claimed as removed by it.
  if (node == nullptr || node->status() != ResourceStatus::Unavailable) {
    return false;
  }
  const FenceEntry* entry = fence_.entry(record.target());
  return entry != nullptr && entry->closed && entry->drain == record.id();
}

Status DrainEngine::add_resource(ResourceNode node, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status added = topology_.add_resource(std::move(node));
  if (!added.ok()) {
    return added;
  }
  record_decision(make_decision("add-resource", DecisionOutcome::Applied, DrainId{}, DrainSetId{}, Generation{},
                                now));
  return ok_status();
}

Status DrainEngine::add_edge(const ResourceId& a, const ResourceId& b, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status added = topology_.add_edge(a, b);
  if (!added.ok()) {
    return added;
  }
  record_decision(make_decision("add-edge", DecisionOutcome::Applied, DrainId{}, DrainSetId{}, Generation{}, now));
  return ok_status();
}

Status DrainEngine::add_path(FabricPath path, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status added = topology_.add_path(std::move(path));
  if (!added.ok()) {
    return added;
  }
  record_decision(make_decision("add-path", DecisionOutcome::Applied, DrainId{}, DrainSetId{}, Generation{}, now));
  return ok_status();
}

Status DrainEngine::add_diversity_group(DiversityGroup group, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status added = topology_.add_diversity_group(std::move(group));
  if (!added.ok()) {
    return added;
  }
  record_decision(make_decision("add-diversity-group", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                Generation{}, now));
  return ok_status();
}

Status DrainEngine::add_capacity_pool(CapacityPool pool, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status added = topology_.add_capacity_pool(std::move(pool));
  if (!added.ok()) {
    return added;
  }
  record_decision(make_decision("add-capacity-pool", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                Generation{}, now));
  return ok_status();
}

Status DrainEngine::add_protected_route(ProtectedRoute route, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status added = topology_.add_protected_route(std::move(route));
  if (!added.ok()) {
    return added;
  }
  record_decision(make_decision("add-protected-route", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                Generation{}, now));
  return ok_status();
}

Status DrainEngine::report_resource_status(const ResourceId& id, ResourceStatus status, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status applied = topology_.set_status(id, status);
  if (!applied.ok()) {
    return applied;
  }
  Decision decision = make_decision("report-resource-status", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("resource=" + id.str());
  decision.inputs.push_back(std::string("status=") + drain::to_string(status));
  record_decision(std::move(decision));
  return ok_status();
}

Topology DrainEngine::topology_snapshot() const {
  LockGuard guard(mutex_, lock_depth_);
  return topology_;
}

EvidenceFence DrainEngine::evidence_fence(TimePoint now) const {
  return EvidenceFence{authority_.incarnation(), authority_.epoch(), now, policy_.evidence_clock_skew()};
}

Result<Obligation> DrainEngine::admit_obligation(Obligation draft, Generation presented, AuthorityId authority,
                                                 TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status authorized = require_authority(authority, now);
  if (!authorized.ok()) {
    ++stats_.rejected_admissions;
    return authorized.error();
  }
  if (draft.dependencies().empty()) {
    ++stats_.rejected_admissions;
    return Error(ErrorCode::InvalidArgument, "an obligation must depend on at least one target");
  }
  if (draft.dependencies().size() > kMaxDependenciesPerObligation) {
    ++stats_.rejected_admissions;
    return Error(ErrorCode::BoundsExceeded, "obligation dependency count exceeds the supported bound");
  }
  for (const auto& target : draft.dependencies()) {
    Status admissible = fence_.check_admission(target, presented);
    if (!admissible.ok()) {
      ++stats_.rejected_admissions;
      Decision decision = make_decision("admit-obligation", DecisionOutcome::Rejected, DrainId{}, DrainSetId{},
                                        presented, now);
      decision.inputs.push_back("holder=" + draft.holder().str());
      decision.inputs.push_back("target=" + target.to_string());
      decision.inputs.push_back("presented-generation=" + drain::to_string(presented));
      decision.rejected.push_back(admissible.error().to_string());
      record_decision(std::move(decision));
      return admissible.error();
    }
  }
  draft.set_admitted_at(now);
  draft.set_admission_generation(presented);
  auto stored = obligations_.insert(std::move(draft));
  if (!stored.ok()) {
    ++stats_.rejected_admissions;
    return stored.error();
  }
  Decision decision = make_decision("admit-obligation", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    presented, now);
  decision.inputs.push_back("obligation=" + drain::to_string(stored->id()));
  decision.inputs.push_back("holder=" + stored->holder().str());
  decision.inputs.push_back(std::string("kind=") + drain::to_string(stored->kind()));
  decision.inputs.push_back(std::string("protection=") + drain::to_string(stored->protection()));
  decision.inputs.push_back("generation=" + drain::to_string(presented));
  record_decision(std::move(decision));
  return *stored;
}

Status DrainEngine::report_obligation(const ObligationReport& report, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Obligation* obligation = obligations_.find_mutable(report.obligation);
  if (obligation == nullptr) {
    ++stats_.rejected_reports;
    return fail(ErrorCode::NotFound, "no such obligation: " + drain::to_string(report.obligation));
  }
  if (!report.drain.is_zero()) {
    const auto it = drains_.find(report.drain);
    if (it == drains_.end()) {
      ++stats_.rejected_reports;
      return fail(ErrorCode::NotFound, "no such drain: " + drain::to_string(report.drain));
    }
    const DrainRecord& record = it->second;
    const bool current_generation = report.generation == record.generation();
    const bool restore_generation =
        !record.restore_generation().is_zero() && report.generation == record.restore_generation();
    if (!current_generation && !restore_generation) {
      ++stats_.rejected_reports;
      return fail(ErrorCode::StaleGeneration,
                  "obligation report for drain " + drain::to_string(report.drain) + " carries generation " +
                      drain::to_string(report.generation) + " but the drain is at generation " +
                      drain::to_string(record.generation()));
    }
  }
  Status applied = obligation->apply_state(report.expected_revision, report.next, report.evidence, now);
  if (!applied.ok()) {
    ++stats_.rejected_reports;
    return applied;
  }
  if (report.attempt.value() != 0) {
    obligation->set_attempt(report.attempt);
  }
  if (!report.note.empty()) {
    obligation->set_note(truncate(report.note));
  }
  Decision decision = make_decision("report-obligation", DecisionOutcome::Applied, report.drain, DrainSetId{},
                                    report.generation, now);
  decision.inputs.push_back("obligation=" + drain::to_string(report.obligation));
  decision.inputs.push_back(std::string("state=") + drain::to_string(report.next));
  decision.inputs.push_back("expected-revision=" + drain::to_string(report.expected_revision));
  decision.inputs.push_back("resulting-revision=" + drain::to_string(obligation->revision()));
  record_decision(std::move(decision));
  return ok_status();
}

std::vector<Obligation> DrainEngine::obligations() const {
  LockGuard guard(mutex_, lock_depth_);
  std::vector<Obligation> out;
  for (const Obligation* obligation : obligations_.all()) {
    out.push_back(*obligation);
  }
  return out;
}

std::optional<Obligation> DrainEngine::obligation(ObligationId id) const {
  LockGuard guard(mutex_, lock_depth_);
  const Obligation* found = obligations_.find(id);
  if (found == nullptr) {
    return std::nullopt;
  }
  return *found;
}

Result<Evidence> DrainEngine::record_evidence(Evidence draft, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  const EvidenceKey key = draft.key;
  const EvidenceSeq seq = draft.seq;
  auto recorded = evidence_.record(std::move(draft), evidence_fence(now),
                                   policy_.evidence_requires_live_incarnation());
  if (!recorded.ok()) {
    ++stats_.rejected_evidence;
    Decision decision = make_decision("record-evidence", DecisionOutcome::Rejected, key.drain, DrainSetId{},
                                      Generation{}, now);
    decision.inputs.push_back("evidence=" + key.label());
    decision.inputs.push_back("sequence=" + drain::to_string(seq));
    decision.rejected.push_back(recorded.error().to_string());
    record_decision(std::move(decision));
    return recorded.error();
  }
  Decision decision = make_decision("record-evidence", DecisionOutcome::Applied, key.drain, DrainSetId{},
                                    recorded->generation, now);
  decision.inputs.push_back("evidence=" + key.label());
  decision.inputs.push_back("sequence=" + drain::to_string(recorded->seq));
  decision.inputs.push_back("observed-at=" + describe_time(recorded->observed_at));
  decision.evidence.push_back("fresh-until=" + describe_time(saturating_add(recorded->observed_at,
                                                                            policy_.evidence_freshness())));
  record_decision(std::move(decision));
  return *recorded;
}

Status DrainEngine::attest_evidence(const EvidenceKey& key, EvidenceSeq seq, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status attested = evidence_.attest(key, seq, evidence_fence(now));
  if (!attested.ok()) {
    ++stats_.rejected_evidence;
    return attested;
  }
  Decision decision = make_decision("attest-evidence", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("sequence=" + drain::to_string(seq));
  decision.inputs.push_back("stream=" + key.label());
  record_decision(std::move(decision));
  return ok_status();
}

std::vector<Evidence> DrainEngine::evidence_records() const {
  LockGuard guard(mutex_, lock_depth_);
  return evidence_.all();
}

std::vector<DrainRecord> DrainEngine::drains() const {
  LockGuard guard(mutex_, lock_depth_);
  std::vector<DrainRecord> out;
  out.reserve(drains_.size());
  for (const auto& [id, record] : drains_) {
    (void)id;
    out.push_back(record);
  }
  return out;
}

std::optional<DrainRecord> DrainEngine::drain(DrainId id) const {
  LockGuard guard(mutex_, lock_depth_);
  const auto it = drains_.find(id);
  if (it == drains_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<DrainSet> DrainEngine::drain_sets() const {
  LockGuard guard(mutex_, lock_depth_);
  std::vector<DrainSet> out;
  out.reserve(sets_.size());
  for (const auto& [id, set] : sets_) {
    (void)id;
    out.push_back(set);
  }
  return out;
}

std::optional<DrainSet> DrainEngine::drain_set(DrainSetId id) const {
  LockGuard guard(mutex_, lock_depth_);
  const auto it = sets_.find(id);
  if (it == sets_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<Blocker> DrainEngine::blockers(DrainId id) const {
  LockGuard guard(mutex_, lock_depth_);
  const auto it = drains_.find(id);
  return it == drains_.end() ? std::vector<Blocker>{} : it->second.blockers();
}

std::vector<ExceptionGrant> DrainEngine::exceptions(DrainId id) const {
  LockGuard guard(mutex_, lock_depth_);
  const auto it = exceptions_.find(id);
  return it == exceptions_.end() ? std::vector<ExceptionGrant>{} : it->second;
}

std::size_t DrainEngine::outstanding_protected_count(const DrainTarget& target) const {
  LockGuard guard(mutex_, lock_depth_);
  return obligations_.outstanding_protected_dependents_of(target).size();
}

std::vector<Obligation> DrainEngine::outstanding_obligations(const DrainTarget& target) const {
  LockGuard guard(mutex_, lock_depth_);
  std::vector<Obligation> out;
  for (const Obligation* obligation : obligations_.outstanding_protected_dependents_of(target)) {
    out.push_back(*obligation);
  }
  return out;
}

std::vector<Decision> DrainEngine::decisions(std::size_t limit) const {
  LockGuard guard(mutex_, lock_depth_);
  std::vector<Decision> out;
  const std::size_t count = std::min(limit, decisions_.size());
  out.reserve(count);
  for (std::size_t index = decisions_.size() - count; index < decisions_.size(); ++index) {
    out.push_back(decisions_[index]);
  }
  return out;
}

EngineStats DrainEngine::stats() const {
  LockGuard guard(mutex_, lock_depth_);
  return stats_;
}

Result<DrainId> DrainEngine::request_drain(const DrainRequest& request, TimePoint now) {
  DrainRequest single = request;
  auto set_id = request_drain_set(single, now);
  if (!set_id.ok()) {
    return set_id.error();
  }
  LockGuard guard(mutex_, lock_depth_);
  const auto it = sets_.find(*set_id);
  if (it == sets_.end() || it->second.members.size() != 1) {
    return Error(ErrorCode::Internal, "single-target drain request did not produce exactly one drain");
  }
  return it->second.members.front();
}

Result<DrainSetId> DrainEngine::request_drain_set(const DrainRequest& request, TimePoint now) {
  auto normalized = normalize_targets(request.targets, policy_.max_targets_per_request());
  if (!normalized.ok()) {
    return normalized.error();
  }
  LockGuard guard(mutex_, lock_depth_);
  Status authorized = require_authority(request.authority, now);
  if (!authorized.ok()) {
    Decision decision = make_decision("request-drain-set", DecisionOutcome::Rejected, DrainId{}, DrainSetId{},
                                      Generation{}, now);
    decision.rejected.push_back(truncate(authorized.error().message()));
    record_decision(std::move(decision));
    return authorized.error();
  }

  if (normalized->size() > policy_.max_drains_in_set()) {
    return Error(ErrorCode::BoundsExceeded, "drain set exceeds the policy member limit");
  }
  if (sets_.size() >= kMaxDrainSets || drains_.size() + normalized->size() > kMaxDrains) {
    return Error(ErrorCode::BoundsExceeded, "drain table is at capacity");
  }

  // Idempotency comes before ownership: replaying a request must return the
  // original set rather than being rejected by the drain that it created.
  if (!request.nonce.is_zero()) {
    const auto existing = nonce_sets_.find(request.nonce);
    if (existing != nonce_sets_.end()) {
      const auto set_it = sets_.find(existing->second);
      if (set_it != sets_.end() && set_it->second.targets == *normalized) {
        return existing->second;
      }
      return Error(ErrorCode::DuplicateIdentity,
                   "request nonce was already used for a different target set");
    }
  }

  // One target has exactly one owner at a time. A target may only be requested
  // again after the previous drain has been fully restored, which is the point
  // at which its admission fence has been reopened under a fresh generation.
  for (const auto& target : *normalized) {
    for (const auto& [existing_id, existing] : drains_) {
      if (!(existing.target() == target)) {
        continue;
      }
      const bool reusable = existing.state() == DrainState::Cancelled && existing.restored_at().count() != 0;
      if (!reusable) {
        return Error(ErrorCode::DuplicateIdentity,
                     "target " + target.to_string() + " is already owned by drain " +
                         drain::to_string(existing_id) + " in state " +
                         drain::to_string(existing.state()));
      }
    }
  }

  // Correlated admission: the whole set must be removable without violating a
  // declared capacity, diversity, or connectivity commitment.
  std::set<ResourceId> removal;
  bool complete_topology = true;
  for (const auto& target : *normalized) {
    if (topology_.resource(target.id()) == nullptr) {
      complete_topology = false;
    }
    removal.insert(target.id());
  }
  if (complete_topology) {
    const TopologyViolation violation = topology_.validate_removal_many(
        removal, policy_.capacity_headroom_numerator(), policy_.capacity_headroom_denominator());
    if (!violation.ok()) {
      ErrorCode code = ErrorCode::RedundancyViolation;
      if (violation.kind == TopologyViolation::Kind::CapacityPoolExhausted ||
          violation.kind == TopologyViolation::Kind::ReservationOverCapacity) {
        code = ErrorCode::CapacityViolation;
      }
      Decision decision = make_decision("request-drain-set", DecisionOutcome::Rejected, DrainId{}, DrainSetId{},
                                        Generation{}, now);
      decision.rejected.push_back(violation.detail);
      decision.rejected.push_back("consider draining fewer targets in the same failure domain");
      record_decision(std::move(decision));
      return Error(code, "correlated drain would violate " + violation.subject + ": " + violation.detail);
    }
  }

  // Failure-domain limits are evaluated as a set-level constraint before any
  // member closes admission.
  std::map<DomainId, std::size_t> per_domain;
  for (const auto& target : *normalized) {
    ++per_domain[target.domain()];
  }
  for (const auto& [domain, count] : per_domain) {
    if (count > policy_.max_concurrent_drains_per_domain()) {
      Decision decision = make_decision("request-drain-set", DecisionOutcome::Rejected, DrainId{}, DrainSetId{},
                                        Generation{}, now);
      decision.rejected.push_back("failure domain " + (domain.empty() ? std::string("<none>") : domain.str()) +
                                  " would host " + std::to_string(count) + " correlated drains");
      record_decision(std::move(decision));
      return Error(ErrorCode::DomainLimitViolation,
                   "the drain set exceeds the failure-domain concurrency limit");
    }
  }

  const auto next_set = increment_checked(set_high_water_.value());
  if (!next_set.has_value()) {
    return Error(ErrorCode::BoundsExceeded, "drain set id space exhausted");
  }
  set_high_water_ = DrainSetId{*next_set};
  DrainSet set;
  set.id = set_high_water_;
  set.reason = truncate(request.reason);
  set.requested_at = now;
  set.authority = request.authority;
  set.nonce = request.nonce;
  set.targets = *normalized;

  for (const auto& target : *normalized) {
    const auto next_drain = increment_checked(drain_high_water_.value());
    if (!next_drain.has_value()) {
      return Error(ErrorCode::BoundsExceeded, "drain id space exhausted");
    }
    drain_high_water_ = DrainId{*next_drain};
    DrainRecord record(drain_high_water_, target, Generation{}, request.authority, now);
    record.set_set(set.id);
    record.set_node(request.node);
    record.set_reason(set.reason);
    set.members.push_back(record.id());
    drains_.emplace(record.id(), std::move(record));
  }
  sets_.emplace(set.id, set);
  if (!request.nonce.is_zero()) {
    nonce_sets_.emplace(request.nonce, set.id);
  }

  Decision decision = make_decision("request-drain-set", DecisionOutcome::Applied, DrainId{}, set.id, Generation{},
                                    now);
  decision.inputs.push_back("set=" + drain::to_string(set.id));
  decision.inputs.push_back("members=" + std::to_string(set.members.size()));
  for (const auto& target : set.targets) {
    decision.inputs.push_back("target=" + target.to_string());
  }
  decision.rejected.push_back("admit-without-correlated-check");
  record_decision(std::move(decision));

  for (const DrainId member : set.members) {
    Decision member_decision = make_decision("request-drain", DecisionOutcome::Applied, member, set.id,
                                             Generation{}, now);
    const auto it = drains_.find(member);
    if (it != drains_.end()) {
      member_decision.inputs.push_back("target=" + it->second.target().to_string());
    }
    member_decision.inputs.push_back("reason=" + set.reason);
    record_decision(std::move(member_decision));
  }
  return set.id;
}

Status DrainEngine::cancel_drain(DrainId id, AuthorityId authority, std::string reason, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status authorized = require_authority(authority, now);
  if (!authorized.ok()) {
    return authorized;
  }
  const auto it = drains_.find(id);
  if (it == drains_.end()) {
    return fail(ErrorCode::NotFound, "no such drain: " + drain::to_string(id));
  }
  DrainRecord& record = it->second;
  if (record.state() == DrainState::Drained) {
    return fail(ErrorCode::IllegalTransition,
                "a drained target must be restored, not cancelled: " + drain::to_string(id));
  }
  if (record.state() == DrainState::Cancelled) {
    return ok_status();
  }
  if (!apply_transition(record, DrainState::Cancelled, now, truncate(reason), DecisionOutcome::Applied,
                        {"reason=" + truncate(reason)}, {}, {"complete-the-drain"})) {
    return fail(ErrorCode::IllegalTransition,
                "drain " + drain::to_string(id) + " cannot be cancelled from state " +
                    drain::to_string(record.state()));
  }
  // Cancellation deliberately leaves the admission fence closed: a cancelled
  // drain does not by itself return the target to service. Restoration is a
  // separate operation that reopens admission under a fresh generation.
  return ok_status();
}

Status DrainEngine::restore_drain(DrainId id, AuthorityId authority, std::string reason, TimePoint now) {
  PendingActions actions;
  {
    LockGuard guard(mutex_, lock_depth_);
    Status authorized = require_authority(authority, now);
    if (!authorized.ok()) {
      return authorized;
    }
    const auto it = drains_.find(id);
    if (it == drains_.end()) {
      return fail(ErrorCode::NotFound, "no such drain: " + drain::to_string(id));
    }
    DrainRecord& record = it->second;
    if (record.state() == DrainState::Restoring) {
      return ok_status();
    }
    if (!is_settled(record.state())) {
      return fail(ErrorCode::IllegalTransition,
                  "only a settled drain can be restored: " + drain::to_string(id) + " is " +
                      drain::to_string(record.state()));
    }
    if (record.state() == DrainState::Cancelled && record.restored_at().count() != 0) {
      return ok_status();
    }

    Generation reopened{};
    if (fence_.is_closed(record.target())) {
      const FenceEntry* entry = fence_.entry(record.target());
      if (entry != nullptr && entry->drain != record.id()) {
        return fail(ErrorCode::IllegalTransition,
                    "admission for " + record.target().to_string() + " is held by drain " +
                        drain::to_string(entry->drain) + ", not by " + drain::to_string(record.id()));
      }
      auto result = fence_.reopen(record.target(), record.id(), now, truncate(reason));
      if (!result.ok()) {
        return result.error();
      }
      reopened = *result;
    } else if (fence_.generation(record.target()) <= record.generation()) {
      // Admission never closed for this drain, so force a generation advance:
      // restoration must always hand out a strictly newer generation.
      auto closed = fence_.close(record.target(), record.id(), now, "restoration generation advance");
      if (!closed.ok()) {
        return closed.error();
      }
      auto opened = fence_.reopen(record.target(), record.id(), now, truncate(reason));
      if (!opened.ok()) {
        return opened.error();
      }
      reopened = *opened;
    } else {
      reopened = fence_.generation(record.target());
    }

    if (!apply_transition(record, DrainState::Restoring, now, truncate(reason), DecisionOutcome::Applied,
                          {"reopened-generation=" + drain::to_string(reopened)}, {}, {"leave-admission-closed"})) {
      return fail(ErrorCode::IllegalTransition,
                  "drain " + drain::to_string(id) + " cannot be restored from state " +
                      drain::to_string(record.state()));
    }
    record.set_restore_generation(reopened);

    for (const Obligation* obligation : outstanding_for(record.target())) {
      RestorationRequest request;
      request.drain = record.id();
      request.target = record.target();
      request.obligation = obligation->id();
      request.generation = record.generation();
      request.restore_generation = reopened;
      request.at = now;
      request.reason = truncate(reason);
      actions.restorations.push_back(std::move(request));
    }
    AdmissionReopenRequest reopen;
    reopen.target = record.target();
    reopen.generation = reopened;
    reopen.at = now;
    reopen.reason = truncate(reason);
    actions.reopens.push_back(std::move(reopen));
  }
  return dispatch(actions, now);
}

Status DrainEngine::resume_drain(DrainId id, AuthorityId authority, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status authorized = require_authority(authority, now);
  if (!authorized.ok()) {
    return authorized;
  }
  const auto it = drains_.find(id);
  if (it == drains_.end()) {
    return fail(ErrorCode::NotFound, "no such drain: " + drain::to_string(id));
  }
  DrainRecord& record = it->second;
  if (record.state() != DrainState::Blocked) {
    return fail(ErrorCode::IllegalTransition, "drain is not blocked: " + drain::to_string(id));
  }
  const DrainState resume = record.blocked_from().value_or(DrainState::Validating);
  auto blockers = evaluate_state(record, resume, now);
  if (!blockers.empty()) {
    const std::string first = blockers.front().render();
    enter_blocked(record, std::move(blockers), now, "resume-rejected");
    return fail(ErrorCode::Blocked, "drain remains blocked: " + first);
  }
  if (!apply_transition(record, resume, now, "operator resume", DecisionOutcome::Applied,
                        {"operator=" + drain::to_string(authority)}, {}, {})) {
    return fail(ErrorCode::IllegalTransition, "drain could not be resumed");
  }
  return ok_status();
}

Status DrainEngine::grant_exception(DrainId id, ObligationId obligation, Duration extension,
                                    AuthorityId authority, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  Status authorized = require_authority(authority, now);
  if (!authorized.ok()) {
    return authorized;
  }
  if (extension.count() <= 0) {
    return fail(ErrorCode::InvalidArgument, "an exception extension must be positive");
  }
  const auto it = drains_.find(id);
  if (it == drains_.end()) {
    return fail(ErrorCode::NotFound, "no such drain: " + drain::to_string(id));
  }
  DrainRecord& record = it->second;
  if (is_settled(record.state())) {
    return fail(ErrorCode::IllegalTransition,
                "a settled drain cannot receive an exception: " + drain::to_string(id));
  }
  if (!obligation.is_zero() && obligations_.find(obligation) == nullptr) {
    return fail(ErrorCode::NotFound, "no such obligation: " + drain::to_string(obligation));
  }
  auto& grants = exceptions_[id];
  if (grants.size() >= policy_.max_exception_extensions()) {
    return fail(ErrorCode::PolicyViolation, "the exception budget for this drain is exhausted");
  }
  ExceptionGrant grant;
  grant.drain = id;
  grant.obligation = obligation;
  grant.extension = extension;
  grant.authority = authority;
  grant.granted_at = now;
  const auto next = increment_checked(exception_high_water_);
  if (!next.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "exception sequence space exhausted");
  }
  exception_high_water_ = *next;
  grant.sequence = exception_high_water_;
  grants.push_back(grant);
  // An exception is the only sanctioned way to obtain further automatic
  // evacuation retries.
  record.reset_evacuation_attempts();

  Decision decision = make_decision("grant-exception", DecisionOutcome::Applied, id, record.set(),
                                    record.generation(), now);
  decision.inputs.push_back("obligation=" + drain::to_string(obligation));
  decision.inputs.push_back("extension=" + describe_duration(extension));
  decision.inputs.push_back("sequence=" + std::to_string(exception_high_water_));
  decision.rejected.push_back("ignore-the-grace-window");
  record_decision(std::move(decision));
  return ok_status();
}

Status DrainEngine::advance(TimePoint now) {
  PendingActions actions;
  std::vector<std::pair<DrainId, bool>> evacuation_results;
  {
    LockGuard guard(mutex_, lock_depth_);
    ++stats_.advances;
    for (auto& [id, record] : drains_) {
      (void)id;
      if (is_settled(record.state())) {
        continue;
      }
      step_drain(record, now, actions);
    }
    for (const auto& request : actions.evacuations) {
      evacuation_results.emplace_back(request.drain, false);
    }
  }
  Status overall = ok_status();
  if (sink_ != nullptr && (!actions.evacuations.empty() || !actions.restorations.empty() ||
                          !actions.reopens.empty())) {
    std::size_t index = 0;
    for (const auto& request : actions.evacuations) {
      const Status sent = sink_->on_evacuation_request(request);
      evacuation_results[index].second = sent.ok();
      ++index;
      if (!sent.ok()) {
        overall = sent;
      }
    }
    for (const auto& request : actions.restorations) {
      const Status sent = sink_->on_restoration_request(request);
      if (!sent.ok()) {
        overall = sent;
      }
    }
    for (const auto& request : actions.reopens) {
      const Status sent = sink_->on_admission_reopen(request);
      if (!sent.ok()) {
        overall = sent;
      }
    }
  }
  {
    LockGuard guard(mutex_, lock_depth_);
    stats_.evacuation_requests += actions.evacuations.size();
    stats_.restoration_requests += actions.restorations.size();
    stats_.reopen_requests += actions.reopens.size();
    for (const auto& [drain_id, ok] : evacuation_results) {
      (void)drain_id;
      if (!ok) {
        ++stats_.sink_failures;
      }
    }
  }
  return overall;
}

Status DrainEngine::dispatch(PendingActions& actions, TimePoint now) {
  (void)now;
  Status overall = ok_status();
  std::size_t failures = 0;
  if (sink_ != nullptr) {
    for (const auto& request : actions.evacuations) {
      const Status sent = sink_->on_evacuation_request(request);
      if (!sent.ok()) {
        ++failures;
        overall = sent;
      }
    }
    for (const auto& request : actions.restorations) {
      const Status sent = sink_->on_restoration_request(request);
      if (!sent.ok()) {
        ++failures;
        overall = sent;
      }
    }
    for (const auto& request : actions.reopens) {
      const Status sent = sink_->on_admission_reopen(request);
      if (!sent.ok()) {
        ++failures;
        overall = sent;
      }
    }
  }
  LockGuard guard(mutex_, lock_depth_);
  stats_.evacuation_requests += actions.evacuations.size();
  stats_.restoration_requests += actions.restorations.size();
  stats_.reopen_requests += actions.reopens.size();
  stats_.sink_failures += failures;
  return overall;
}

Status DrainEngine::record_removal_evidence(const DrainRecord& record, TimePoint now) {
  const EvidenceKey key{EvidenceKind::ResourceRemoved, record.id(), ObligationId{}, record.target().id()};
  const auto next_seq = increment_checked(evidence_.high_water(key).value());
  if (!next_seq.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "the evidence sequence space is exhausted");
  }
  Evidence draft;
  draft.seq = EvidenceSeq{*next_seq};
  draft.key = key;
  draft.generation = record.generation();
  draft.epoch = authority_.epoch();
  draft.producer = authority_.incarnation();
  draft.authority = record.authority();
  draft.observed_at = now;
  draft.expires_at = saturating_add(now, policy_.evidence_freshness());
  draft.healthy = true;
  draft.detail = "removal observation taken by drain " + drain::to_string(record.id());
  auto recorded = evidence_.record(std::move(draft), evidence_fence(now),
                                   policy_.evidence_requires_live_incarnation());
  if (!recorded.ok()) {
    return recorded.error();
  }
  return ok_status();
}

bool DrainEngine::removal_is_reobservable(const DrainRecord& record) const {
  if (!outstanding_for(record.target()).empty()) {
    return false;
  }
  const FenceEntry* entry = fence_.entry(record.target());
  if (entry == nullptr || !entry->closed || entry->generation != record.generation()) {
    return false;
  }
  const ResourceNode* node = topology_.resource(record.target().id());
  return node != nullptr && node->status() == ResourceStatus::Unavailable;
}

std::vector<const Obligation*> DrainEngine::outstanding_for(const DrainTarget& target) const {
  return obligations_.outstanding_protected_dependents_of(target);
}

Blocker DrainEngine::make_blocker(BlockerCode code, std::string subject, std::string detail, TimePoint since,
                                  TimePoint deadline) const {
  Blocker blocker;
  blocker.code = code;
  blocker.subject = truncate(std::move(subject), 192);
  blocker.detail = truncate(std::move(detail));
  blocker.since = since;
  blocker.deadline = deadline;
  return blocker;
}

Blocker DrainEngine::blocker_from_violation(const DrainRecord& record, const TopologyViolation& violation,
                                            TimePoint now) const {
  BlockerCode code = BlockerCode::TopologyInconsistent;
  switch (violation.kind) {
    case TopologyViolation::Kind::None:
    case TopologyViolation::Kind::UnknownResource:
      code = BlockerCode::TopologyInconsistent;
      break;
    case TopologyViolation::Kind::CapacityPoolExhausted:
    case TopologyViolation::Kind::ReservationOverCapacity:
      code = BlockerCode::CapacityInsufficient;
      break;
    case TopologyViolation::Kind::DiversityLost:
      code = BlockerCode::PathDiversityViolation;
      break;
    case TopologyViolation::Kind::RouteSevered:
      code = BlockerCode::AlternatePathLost;
      break;
  }
  std::string detail = violation.detail;
  detail.append(" required=");
  detail.append(std::to_string(violation.required));
  detail.append(" remaining=");
  detail.append(std::to_string(violation.remaining));
  const std::string subject =
      violation.subject.empty() ? record.target().to_string() : violation.subject;
  return make_blocker(code, subject, std::move(detail), now, TimePoint{0});
}

TimePoint DrainEngine::grace_deadline_for(const DrainRecord& record, const Obligation& obligation) const {
  TimePoint deadline = saturating_add(obligation.admitted_at(), policy_.grace_for(obligation));
  const auto it = exceptions_.find(record.id());
  if (it != exceptions_.end()) {
    for (const auto& grant : it->second) {
      if (grant.obligation.is_zero() || grant.obligation == obligation.id()) {
        deadline = saturating_add(deadline, grant.extension);
      }
    }
  }
  return deadline;
}

std::size_t DrainEngine::active_drains_in_domain(const DomainId& domain, DrainId exclude) const {
  std::size_t count = 0;
  for (const auto& [id, record] : drains_) {
    if (id == exclude) {
      continue;
    }
    if (record.target().domain() != domain) {
      continue;
    }
    if (holds_fence(record)) {
      ++count;
    }
  }
  return count;
}

Decision DrainEngine::make_decision(std::string action, DecisionOutcome outcome, DrainId drain, DrainSetId set,
                                    Generation generation, TimePoint now) const {
  Decision decision;
  decision.action = std::move(action);
  decision.outcome = outcome;
  decision.drain = drain;
  decision.set = set;
  decision.generation = generation;
  decision.epoch = authority_.epoch();
  decision.incarnation = authority_.incarnation();
  decision.policy_revision = policy_.revision();
  decision.policy_fingerprint = policy_.fingerprint();
  decision.at = now;
  return decision;
}

void DrainEngine::record_decision(Decision decision) {
  decisions_.push_back(std::move(decision));
  while (decisions_.size() > policy_.max_decisions_retained()) {
    decisions_.pop_front();
  }
}

bool DrainEngine::apply_transition(DrainRecord& record, DrainState next, TimePoint now, std::string detail,
                                   DecisionOutcome outcome, std::vector<std::string> inputs,
                                   std::vector<std::string> evidence_notes,
                                   std::vector<std::string> rejected) {
  const DrainState previous = record.state();
  Status applied = record.transition(next, now, detail);
  if (!applied.ok()) {
    Decision rejected_decision = make_decision("transition-rejected", DecisionOutcome::Rejected, record.id(),
                                               record.set(), record.generation(), now);
    rejected_decision.inputs.push_back("from=" + std::string(drain::to_string(previous)));
    rejected_decision.inputs.push_back("to=" + std::string(drain::to_string(next)));
    rejected_decision.rejected.push_back(applied.error().to_string());
    record_decision(std::move(rejected_decision));
    return false;
  }
  ++stats_.transitions;
  Decision decision = make_decision(transition_action(next), outcome, record.id(), record.set(),
                                    record.generation(), now);
  decision.inputs.push_back("from=" + std::string(drain::to_string(previous)));
  decision.inputs.push_back("detail=" + truncate(std::move(detail)));
  for (auto& entry : inputs) {
    decision.inputs.push_back(std::move(entry));
  }
  decision.evidence = std::move(evidence_notes);
  decision.rejected = std::move(rejected);
  if (next == DrainState::Blocked) {
    decision.blockers = record.blockers();
  }
  record_decision(std::move(decision));
  return true;
}

bool DrainEngine::enter_blocked(DrainRecord& record, std::vector<Blocker> blockers, TimePoint now,
                                std::string action) {
  std::sort(blockers.begin(), blockers.end());
  blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
  if (blockers.size() > policy_.max_blockers_per_drain()) {
    blockers.resize(policy_.max_blockers_per_drain());
  }
  record.set_blockers(std::move(blockers));
  ++stats_.blocked_evaluations;
  if (record.state() == DrainState::Blocked) {
    Decision decision = make_decision(std::move(action), DecisionOutcome::Blocked, record.id(), record.set(),
                                      record.generation(), now);
    decision.blockers = record.blockers();
    record_decision(std::move(decision));
    return false;
  }
  return apply_transition(record, DrainState::Blocked, now, std::move(action), DecisionOutcome::Blocked, {}, {},
                          {"ignore-the-blocker"});
}

std::vector<Blocker> DrainEngine::evaluate_start(const DrainRecord& record, TimePoint now) const {
  std::vector<Blocker> blockers;
  if (record.authority().is_zero()) {
    blockers.push_back(make_blocker(BlockerCode::AuthorityInvalid, "authority:none",
                                    "the drain was requested without an authority token", now, TimePoint{0}));
  } else {
    // The token is re-checked only while it belongs to the live incarnation. A
    // drain requested before a restart was accepted by an incarnation that no
    // longer exists; its durability is the authority, and from here its
    // progress is governed by the admission fence it will close. Revoking a
    // token in the incarnation that issued it still stops a drain that has not
    // yet closed admission, which is the operator control that matters.
    const AuthorityToken* token = authority_.find(record.authority());
    const bool issued_by_live_incarnation =
        token != nullptr && token->incarnation() == authority_.incarnation();
    if (issued_by_live_incarnation) {
      const Status authorized = require_authority(record.authority(), now);
      if (!authorized.ok()) {
        blockers.push_back(make_blocker(BlockerCode::AuthorityInvalid,
                                        "authority:" + drain::to_string(record.authority()),
                                        authorized.error().message(), now, TimePoint{0}));
      }
    }
  }

  // A drain may only close admission for a target it owns.
  const FenceEntry* fence_entry = fence_.entry(record.target());
  if (fence_entry != nullptr && fence_entry->closed && fence_entry->drain != record.id()) {
    blockers.push_back(make_blocker(BlockerCode::GenerationSuperseded, record.target().to_string(),
                                    "another drain holds the admission fence for this target", now,
                                    TimePoint{0}));
  }

  const ResourceNode* node = topology_.resource(record.target().id());
  if (node == nullptr) {
    blockers.push_back(make_blocker(BlockerCode::TopologyInconsistent, record.target().to_string(),
                                    "the target is not present in the topology", now, TimePoint{0}));
  } else if (!node->in_service()) {
    blockers.push_back(make_blocker(BlockerCode::TopologyInconsistent, record.target().to_string(),
                                    "the target is already out of service", now, TimePoint{0}));
  }

  const std::size_t holding = active_drains_in_domain(record.target().domain(), record.id());
  if (holding >= policy_.max_concurrent_drains_per_domain()) {
    blockers.push_back(make_blocker(
        BlockerCode::DomainLimitReached,
        "domain:" + (record.target().domain().empty() ? std::string("<none>") : record.target().domain().str()),
        "failure domain already hosts " + std::to_string(holding) + " drain(s) holding an admission fence",
        now, TimePoint{0}));
  }

  // Deterministic admission order inside a failure domain: only the
  // lowest-numbered drain that has not yet closed admission may proceed. This
  // keeps correlated admission deadlock free and reproducible.
  DrainId frontmost{};
  bool found_front = false;
  for (const auto& [id, other] : drains_) {
    if (id == record.id() || other.target().domain() != record.target().domain()) {
      continue;
    }
    const bool waiting = other.state() == DrainState::Requested || other.state() == DrainState::Validating ||
                         (other.state() == DrainState::Blocked && other.blocked_from().has_value() &&
                          (*other.blocked_from() == DrainState::Requested ||
                           *other.blocked_from() == DrainState::Validating));
    if (!waiting) {
      continue;
    }
    if (!found_front || id < frontmost) {
      frontmost = id;
      found_front = true;
    }
  }
  if (found_front && frontmost < record.id()) {
    blockers.push_back(make_blocker(BlockerCode::SetPredecessorPending, "drain:" + drain::to_string(frontmost),
                                    "an earlier drain in the same failure domain has not closed admission yet",
                                    now, TimePoint{0}));
  }

  if (!record.set().is_zero() && policy_.serialize_same_domain_in_set()) {
    const auto set_it = sets_.find(record.set());
    if (set_it != sets_.end()) {
      for (const DrainId member : set_it->second.members) {
        if (member == record.id()) {
          break;
        }
        const auto other = drains_.find(member);
        if (other == drains_.end() || other->second.target().domain() != record.target().domain()) {
          continue;
        }
        if (!is_settled(other->second.state())) {
          blockers.push_back(make_blocker(BlockerCode::SetPredecessorPending,
                                          "drain:" + drain::to_string(member),
                                          "predecessor in the same failure domain has not settled", now,
                                          TimePoint{0}));
          break;
        }
      }
    }
  }

  if (node != nullptr && node->in_service()) {
    const TopologyViolation violation =
        topology_.validate_removal(record.target().id(), policy_.capacity_headroom_numerator(),
                                   policy_.capacity_headroom_denominator());
    if (!violation.ok()) {
      blockers.push_back(blocker_from_violation(record, violation, now));
    }
  }
  return blockers;
}

std::vector<Blocker> DrainEngine::evaluate_evacuation(const DrainRecord& record, TimePoint now) const {
  std::vector<Blocker> blockers;
  bool movable = false;
  for (const Obligation* obligation : outstanding_for(record.target())) {
    if (obligation->state() == ObligationState::Failed) {
      blockers.push_back(make_blocker(BlockerCode::ObligationFailed, obligation->label(),
                                      "the adjacent runtime reported that evacuation failed", now,
                                      TimePoint{0}));
      continue;
    }
    if (obligation->state() == ObligationState::Admitted ||
        obligation->state() == ObligationState::Evacuating) {
      movable = true;
    }
    const TimePoint deadline = grace_deadline_for(record, *obligation);
    if (now > deadline) {
      blockers.push_back(make_blocker(BlockerCode::ObligationGraceExpired, obligation->label(),
                                      "the policy grace window expired without a release report", now,
                                      deadline));
    }
  }
  if (movable && record.evacuation_requests() >= policy_.max_evacuation_attempts()) {
    blockers.push_back(make_blocker(BlockerCode::EvacuationAttemptsExhausted,
                                    "drain:" + drain::to_string(record.id()),
                                    "automatic evacuation retries are exhausted; an exception grant or a "
                                    "manual release report is required",
                                    now, TimePoint{0}));
  }
  return blockers;
}

std::vector<Blocker> DrainEngine::evaluate_quiescence(DrainRecord& record, TimePoint now,
                                                      std::vector<ObligationId>& retirable) const {
  std::vector<Blocker> blockers;
  retirable.clear();
  const auto fence = evidence_fence(now);
  for (const Obligation* obligation : outstanding_for(record.target())) {
    switch (obligation->state()) {
      case ObligationState::Admitted:
      case ObligationState::Evacuating:
        blockers.push_back(make_blocker(BlockerCode::ProtectedObligationActive, obligation->label(),
                                        "the obligation has not been evacuated yet", now,
                                        grace_deadline_for(record, *obligation)));
        break;
      case ObligationState::Quiescing:
      case ObligationState::Released: {
        const EvidenceSeq seq = obligation->release_evidence();
        if (seq.is_zero()) {
          blockers.push_back(make_blocker(BlockerCode::ReleaseEvidenceMissing, obligation->label(),
                                          "no release evidence was supplied with the release report", now,
                                          TimePoint{0}));
          break;
        }
        const EvidenceKey release_key{EvidenceKind::ObligationReleased, record.id(), obligation->id(),
                                      ResourceId{}};
        const Evidence* record_evidence = evidence_.find(release_key, seq);
        if (record_evidence == nullptr) {
          blockers.push_back(make_blocker(BlockerCode::ReleaseEvidenceMissing, obligation->label(),
                                          "the referenced release evidence record does not exist", now,
                                          TimePoint{0}));
          break;
        }
        if (!evidence_.is_fresh(*record_evidence, fence, policy_.evidence_freshness())) {
          blockers.push_back(make_blocker(
              BlockerCode::ReleaseEvidenceStale, obligation->label(),
              "the release evidence is quarantined, superseded, or older than the freshness window", now,
              saturating_add(record_evidence->observed_at, policy_.evidence_freshness())));
          break;
        }
        if (record_evidence->generation != record.generation()) {
          blockers.push_back(make_blocker(BlockerCode::GenerationSuperseded, obligation->label(),
                                          "the release evidence belongs to a different drain generation", now,
                                          TimePoint{0}));
          break;
        }
        if (record_evidence->observed_at < record.admission_closed_at()) {
          blockers.push_back(make_blocker(
              BlockerCode::ReleaseEvidenceStale, obligation->label(),
              "the release evidence predates the admission boundary and cannot prove post-fence quiescence",
              now, TimePoint{0}));
          break;
        }
        retirable.push_back(obligation->id());
        break;
      }
      case ObligationState::Failed:
        blockers.push_back(make_blocker(BlockerCode::ObligationFailed, obligation->label(),
                                        "the adjacent runtime reported that evacuation failed", now,
                                        TimePoint{0}));
        break;
      case ObligationState::Retired:
        break;
    }
  }
  if (!blockers.empty() || !retirable.empty()) {
    return blockers;
  }
  // With nothing left to retire, the next thing the lifecycle will do is the
  // removal step. Its validation belongs here too, otherwise a drain blocked by
  // it would oscillate between Blocked and Quiescing instead of holding a
  // stable, explainable blocker.
  const ResourceNode* node = topology_.resource(record.target().id());
  if (node == nullptr) {
    blockers.push_back(make_blocker(BlockerCode::TopologyInconsistent, record.target().to_string(),
                                    "the target disappeared before the removal step", now, TimePoint{0}));
    return blockers;
  }
  if (node->status() != ResourceStatus::InService) {
    if (!removal_owned_by(record)) {
      blockers.push_back(make_blocker(
          BlockerCode::TopologyInconsistent, record.target().to_string(),
          "the target left service outside this drain, so this drain cannot claim its removal", now,
          TimePoint{0}));
    }
    return blockers;
  }
  const TopologyViolation violation =
      topology_.validate_removal(record.target().id(), policy_.capacity_headroom_numerator(),
                                 policy_.capacity_headroom_denominator());
  if (!violation.ok()) {
    blockers.push_back(blocker_from_violation(record, violation, now));
  }
  return blockers;
}

std::vector<Blocker> DrainEngine::evaluate_verification(const DrainRecord& record, TimePoint now,
                                                        EvidenceSeq& proof) const {
  std::vector<Blocker> blockers;
  proof = EvidenceSeq{};
  for (const Obligation* obligation : outstanding_for(record.target())) {
    blockers.push_back(make_blocker(BlockerCode::ProtectedObligationActive, obligation->label(),
                                    "a protected obligation still depends on the target", now, TimePoint{0}));
  }
  if (!blockers.empty()) {
    return blockers;
  }
  const ResourceNode* node = topology_.resource(record.target().id());
  if (node == nullptr) {
    blockers.push_back(make_blocker(BlockerCode::TopologyInconsistent, record.target().to_string(),
                                    "the target disappeared from the topology during verification", now,
                                    TimePoint{0}));
  } else if (!removal_owned_by(record)) {
    blockers.push_back(make_blocker(BlockerCode::TopologyInconsistent, record.target().to_string(),
                                    "the removal step of this drain has not taken the target out of "
                                    "service under this drain generation",
                                    now, TimePoint{0}));
  }
  if (policy_.require_quiescence_evidence()) {
    const EvidenceKey key{EvidenceKind::ResourceRemoved, record.id(), ObligationId{}, record.target().id()};
    const Evidence* candidate = evidence_.fresh(key, evidence_fence(now), policy_.evidence_freshness());
    if (candidate == nullptr) {
      blockers.push_back(make_blocker(BlockerCode::QuiescenceEvidenceMissing, key.label(),
                                      "no fresh removal evidence exists for this drain generation", now,
                                      TimePoint{0}));
    } else if (candidate->generation != record.generation()) {
      blockers.push_back(make_blocker(BlockerCode::GenerationSuperseded, key.label(),
                                      "the removal evidence belongs to a different drain generation", now,
                                      TimePoint{0}));
    } else if (candidate->observed_at < record.admission_closed_at()) {
      blockers.push_back(make_blocker(BlockerCode::ReleaseEvidenceStale, key.label(),
                                      "the removal evidence predates the admission boundary", now,
                                      TimePoint{0}));
    } else {
      proof = candidate->seq;
    }
  }
  return blockers;
}

std::vector<Blocker> DrainEngine::evaluate_state(const DrainRecord& record, DrainState state,
                                                 TimePoint now) const {
  std::vector<ObligationId> retirable;
  EvidenceSeq proof{};
  switch (state) {
    case DrainState::Requested:
    case DrainState::Validating:
      return evaluate_start(record, now);
    case DrainState::Evacuating:
      return evaluate_evacuation(record, now);
    case DrainState::Quiescing:
      return evaluate_quiescence(const_cast<DrainRecord&>(record), now, retirable);
    case DrainState::Verifying:
      return evaluate_verification(record, now, proof);
    default:
      return {};
  }
}

void DrainEngine::step_drain(DrainRecord& record, TimePoint now, PendingActions& actions) {
  if (record.state() == DrainState::Blocked) {
    // Revalidation is not a lifecycle step: when the blockers have cleared the
    // drain resumes *and* performs the transition it was waiting for, in the
    // same pass. Otherwise a restart would cost one scheduling pass per
    // recovery.
    const DrainState resume = record.blocked_from().value_or(DrainState::Validating);
    if (resume == DrainState::Verifying && policy_.reobserve_removal_evidence() &&
        removal_is_reobservable(record)) {
      // A restart quarantines the previous incarnation's removal observation.
      // When live authoritative state still proves the removal for this
      // generation, the engine takes a fresh observation instead of trusting
      // the recovered one.
      (void)record_removal_evidence(record, now);
    }
    auto blockers = evaluate_state(record, resume, now);
    if (!blockers.empty()) {
      enter_blocked(record, std::move(blockers), now, "stay-blocked");
      return;
    }
    if (!apply_transition(record, resume, now, "blockers cleared", DecisionOutcome::Applied,
                          {"resume-to=" + std::string(drain::to_string(resume))}, {}, {})) {
      return;
    }
  }
  switch (record.state()) {
    case DrainState::Requested: {
      apply_transition(record, DrainState::Validating, now, "begin validation", DecisionOutcome::Applied, {}, {},
                       {});
      return;
    }
    case DrainState::Validating: {
      auto blockers = evaluate_start(record, now);
      if (!blockers.empty()) {
        enter_blocked(record, std::move(blockers), now, "validation-blocked");
        return;
      }
      auto generation = fence_.close(record.target(), record.id(), now, record.reason());
      if (!generation.ok()) {
        std::vector<Blocker> fence_blockers;
        fence_blockers.push_back(make_blocker(BlockerCode::AdmissionFenceMissing, record.target().to_string(),
                                              generation.error().message(), now, TimePoint{0}));
        enter_blocked(record, std::move(fence_blockers), now, "fence-close-failed");
        return;
      }
      record.set_generation(*generation);
      apply_transition(record, DrainState::AdmissionClosed, now, "admission closed", DecisionOutcome::Applied,
                       {"target=" + record.target().to_string(),
                        "generation=" + drain::to_string(*generation)},
                       {"fence-state=closed"}, {"proceed-without-closing-admission"});
      return;
    }
    case DrainState::AdmissionClosed: {
      if (now < saturating_add(record.admission_closed_at(), policy_.admission_fence_settle())) {
        Decision decision = make_decision("await-fence-settle", DecisionOutcome::NoOp, record.id(),
                                          record.set(), record.generation(), now);
        decision.inputs.push_back("settle=" + describe_duration(policy_.admission_fence_settle()));
        decision.inputs.push_back("closed-at=" + describe_time(record.admission_closed_at()));
        record_decision(std::move(decision));
        return;
      }
      const auto outstanding = outstanding_for(record.target());
      if (outstanding.empty()) {
        apply_transition(record, DrainState::Quiescing, now, "no protected dependencies depend on the target",
                         DecisionOutcome::Applied, {"outstanding-protected=0"}, {},
                         {"request-evacuation-with-nothing-to-evacuate"});
      } else {
        apply_transition(record, DrainState::Evacuating, now, "protected dependencies require evacuation",
                         DecisionOutcome::Applied,
                         {"outstanding-protected=" + std::to_string(outstanding.size())}, {},
                         {"declare-quiescent-with-active-obligations"});
      }
      return;
    }
    case DrainState::Evacuating: {
      const auto outstanding = outstanding_for(record.target());
      bool still_moving = false;
      for (const Obligation* obligation : outstanding) {
        if (obligation->state() == ObligationState::Admitted ||
            obligation->state() == ObligationState::Evacuating) {
          still_moving = true;
        }
      }
      if (!still_moving) {
        // Nothing is left to move: the remaining work is release and
        // quiescence confirmation, which is a different adjacent-runtime
        // operation and therefore a different lifecycle state.
        apply_transition(record, DrainState::Quiescing, now,
                         "every protected dependency has been evacuated", DecisionOutcome::Applied,
                         {"outstanding-protected=" + std::to_string(outstanding.size())}, {}, {});
        return;
      }
      auto blockers = evaluate_evacuation(record, now);
      if (!blockers.empty()) {
        enter_blocked(record, std::move(blockers), now, "evacuation-blocked");
        return;
      }
      const bool never_requested = record.evacuation_requests() == 0;
      const bool retry_due =
          now >= saturating_add(record.last_evacuation_at(), policy_.evacuation_retry_interval());
      if (!never_requested && !retry_due) {
        Decision decision = make_decision("await-evacuation", DecisionOutcome::NoOp, record.id(), record.set(),
                                          record.generation(), now);
        decision.inputs.push_back("outstanding-protected=" + std::to_string(outstanding.size()));
        decision.inputs.push_back("next-retry-after=" +
                                  describe_time(saturating_add(record.last_evacuation_at(),
                                                               policy_.evacuation_retry_interval())));
        record_decision(std::move(decision));
        return;
      }
      const Attempt attempt{static_cast<std::uint32_t>(record.evacuation_requests() + 1)};
      std::size_t issued = 0;
      for (const Obligation* obligation : outstanding) {
        if (obligation->state() != ObligationState::Admitted &&
            obligation->state() != ObligationState::Evacuating) {
          continue;
        }
        EvacuationRequest request;
        request.drain = record.id();
        request.target = record.target();
        request.obligation = obligation->id();
        request.expected_revision = obligation->revision();
        const EvidenceKey release_key{EvidenceKind::ObligationReleased, record.id(), obligation->id(),
                                      ResourceId{}};
        const auto reserved = increment_checked(evidence_.high_water(release_key).value());
        if (reserved.has_value()) {
          request.evidence_seq = EvidenceSeq{*reserved};
        }
        request.kind = obligation->kind();
        request.mode = obligation->evacuation_mode();
        request.generation = record.generation();
        request.attempt = attempt;
        request.at = now;
        request.reason = record.reason();
        actions.evacuations.push_back(std::move(request));
        ++issued;
      }
      record.note_evacuation_attempt(now);
      Decision decision = make_decision("request-evacuation", DecisionOutcome::Applied, record.id(),
                                        record.set(), record.generation(), now);
      decision.inputs.push_back("attempt=" + drain::to_string(attempt));
      decision.inputs.push_back("requested=" + std::to_string(issued));
      decision.inputs.push_back("outstanding-protected=" + std::to_string(outstanding.size()));
      decision.rejected.push_back("perform-the-migration-in-drain-fabric");
      record_decision(std::move(decision));
      return;
    }
    case DrainState::Quiescing: {
      const auto outstanding = outstanding_for(record.target());
      bool needs_evacuation = false;
      for (const Obligation* obligation : outstanding) {
        if (obligation->state() == ObligationState::Admitted ||
            obligation->state() == ObligationState::Evacuating) {
          needs_evacuation = true;
        }
      }
      if (needs_evacuation) {
        apply_transition(record, DrainState::Evacuating, now,
                         "an obligation reappeared and must be evacuated again", DecisionOutcome::Applied,
                         {"reappeared=true"}, {}, {});
        return;
      }
      std::vector<ObligationId> retirable;
      auto blockers = evaluate_quiescence(record, now, retirable);
      if (!blockers.empty()) {
        enter_blocked(record, std::move(blockers), now, "quiescence-blocked");
        return;
      }
      if (!retirable.empty()) {
        std::size_t retired = 0;
        for (const ObligationId id : retirable) {
          Obligation* obligation = obligations_.find_mutable(id);
          if (obligation == nullptr) {
            continue;
          }
          Status applied = obligation->apply_state(obligation->revision(), ObligationState::Retired,
                                                   obligation->release_evidence(), now);
          if (applied.ok()) {
            ++retired;
          }
        }
        Decision decision = make_decision("retire-obligations", DecisionOutcome::Applied, record.id(),
                                          record.set(), record.generation(), now);
        decision.inputs.push_back("retired=" + std::to_string(retired));
        for (const ObligationId id : retirable) {
          const Obligation* obligation = obligations_.find(id);
          if (obligation != nullptr) {
            decision.evidence.push_back(obligation->label() + " release-evidence=" +
                                        drain::to_string(obligation->release_evidence()));
          }
        }
        record_decision(std::move(decision));
        return;
      }

      // Removal step. Capacity and redundancy are validated before the resource
      // leaves service, never after.
      const ResourceNode* node = topology_.resource(record.target().id());
      if (node == nullptr) {
        std::vector<Blocker> missing;
        missing.push_back(make_blocker(BlockerCode::TopologyInconsistent, record.target().to_string(),
                                       "the target disappeared before the removal step", now, TimePoint{0}));
        enter_blocked(record, std::move(missing), now, "removal-step-rejected");
        return;
      }
      if (node->status() != ResourceStatus::InService && !removal_owned_by(record)) {
        std::vector<Blocker> foreign;
        foreign.push_back(make_blocker(
            BlockerCode::TopologyInconsistent, record.target().to_string(),
            "the target left service outside this drain, so this drain cannot claim its removal", now,
            TimePoint{0}));
        enter_blocked(record, std::move(foreign), now, "removal-step-not-owned");
        return;
      }
      if (node->in_service()) {
        const TopologyViolation violation =
            topology_.validate_removal(record.target().id(), policy_.capacity_headroom_numerator(),
                                       policy_.capacity_headroom_denominator());
        if (!violation.ok()) {
          std::vector<Blocker> violation_blockers;
          violation_blockers.push_back(blocker_from_violation(record, violation, now));
          enter_blocked(record, std::move(violation_blockers), now, "removal-step-rejected");
          return;
        }
        ResourceNode* mutable_node = topology_.mutable_resource(record.target().id());
        mutable_node->set_status(ResourceStatus::Unavailable);
      }
      Status observed = record_removal_evidence(record, now);
      if (!observed.ok()) {
        std::vector<Blocker> evidence_blockers;
        evidence_blockers.push_back(make_blocker(
            BlockerCode::QuiescenceEvidenceMissing,
            EvidenceKey{EvidenceKind::ResourceRemoved, record.id(), ObligationId{}, record.target().id()}.label(),
            observed.error().message(), now, TimePoint{0}));
        enter_blocked(record, std::move(evidence_blockers), now, "removal-evidence-failed");
        return;
      }
      apply_transition(record, DrainState::Verifying, now, "removal step completed", DecisionOutcome::Applied,
                       {"resource=" + record.target().id().str(), "removal=applied"},
                       {"removal-observation=recorded-for-generation-" + drain::to_string(record.generation())},
                       {"removal=assumed-without-observation"});
      return;
    }
    case DrainState::Verifying: {
      EvidenceSeq proof{};
      auto blockers = evaluate_verification(record, now, proof);
      if (!blockers.empty() && policy_.reobserve_removal_evidence() && removal_is_reobservable(record)) {
        Status observed = record_removal_evidence(record, now);
        if (observed.ok()) {
          blockers = evaluate_verification(record, now, proof);
        }
      }
      if (!blockers.empty()) {
        enter_blocked(record, std::move(blockers), now, "verification-blocked");
        return;
      }
      const std::size_t remaining = outstanding_for(record.target()).size();
      const std::uint64_t digest = authoritative_digest_locked();
      Status recorded = record.record_completion(now, static_cast<std::uint32_t>(remaining), digest, proof);
      if (!recorded.ok()) {
        std::vector<Blocker> completion_blockers;
        completion_blockers.push_back(make_blocker(BlockerCode::ProtectedObligationActive,
                                                   record.target().to_string(), recorded.error().message(), now,
                                                   TimePoint{0}));
        enter_blocked(record, std::move(completion_blockers), now, "completion-refused");
        return;
      }
      ++stats_.completion_proofs;
      apply_transition(record, DrainState::Drained, now,
                       "authoritative snapshot proves zero protected dependencies", DecisionOutcome::Applied,
                       {"remaining-protected=0", "snapshot-digest=" + hex_u64(digest)},
                       {"removal-evidence=" + drain::to_string(proof)}, {"complete-without-verification"});
      return;
    }
    case DrainState::Blocked:
      return;
    case DrainState::Restoring: {
      if (now < saturating_add(record.state_since(), policy_.admission_fence_settle())) {
        Decision decision = make_decision("await-restore-settle", DecisionOutcome::NoOp, record.id(),
                                          record.set(), record.restore_generation(), now);
        decision.inputs.push_back("settle=" + describe_duration(policy_.admission_fence_settle()));
        record_decision(std::move(decision));
        return;
      }
      if (removal_owned_by(record)) {
        ResourceNode* node = topology_.mutable_resource(record.target().id());
        node->set_status(ResourceStatus::InService);
      }
      if (!apply_transition(record, DrainState::Cancelled, now,
                            "restoration complete; admission reopened under a fresh generation",
                            DecisionOutcome::Applied,
                            {"reopened-generation=" + drain::to_string(record.restore_generation())}, {},
                            {"reopen-with-the-closed-generation"})) {
        enter_blocked(record,
                      {make_blocker(BlockerCode::GenerationSuperseded, record.target().to_string(),
                                    "restoration could not settle into the cancelled state", now, TimePoint{0})},
                      now, "restoration-rejected");
        return;
      }
      record.mark_restored(now, record.restore_generation());
      return;
    }
    case DrainState::Drained:
    case DrainState::Cancelled:
    case DrainState::Failed:
      return;
  }
}

std::uint64_t DrainEngine::authoritative_digest_locked() const {
  Digest64 digest;
  for (const auto& [id, node] : topology_.resources()) {
    digest.update_tagged("resource", id.str());
    digest.update_u64(static_cast<std::uint64_t>(node.status()));
    digest.update_u64(node.capacity().value());
    digest.update_u64(node.reserved().value());
  }
  digest.update_u64(obligations_.digest());
  for (const auto& [id, record] : drains_) {
    digest.update_u64(id.value());
    digest.update_u64(static_cast<std::uint64_t>(record.state()));
    digest.update_u64(record.generation().value());
    digest.update_u64(record.restore_generation().value());
  }
  for (const auto& [target, entry] : fence_.entries()) {
    digest.update_tagged("fence", target.to_string());
    digest.update_bool(entry.closed);
    digest.update_u64(entry.generation.value());
  }
  return digest.value();
}

std::uint64_t DrainEngine::authoritative_digest() const {
  LockGuard guard(mutex_, lock_depth_);
  return authoritative_digest_locked();
}

AccountingReport DrainEngine::audit(TimePoint now) const {
  (void)now;
  LockGuard guard(mutex_, lock_depth_);
  AccountingReport report;
  report.drains_total = drains_.size();
  report.drain_sets = sets_.size();
  report.evidence_records = evidence_.size();
  report.obligations_total = obligations_.size();
  report.authoritative_digest = authoritative_digest_locked();
  report.obligation_digest = obligations_.digest();
  report.evidence_digest = evidence_.digest();
  {
    Digest64 topology_digest;
    for (const auto& [id, node] : topology_.resources()) {
      topology_digest.update_tagged("resource", id.str());
      topology_digest.update_u64(static_cast<std::uint64_t>(node.status()));
    }
    report.topology_digest = topology_digest.value();
  }

  for (const auto& [id, record] : drains_) {
    switch (record.state()) {
      case DrainState::Drained: ++report.drains_drained; break;
      case DrainState::Cancelled:
        ++report.drains_cancelled;
        if (record.restored_at().count() != 0) {
          ++report.drains_restored;
        }
        break;
      case DrainState::Failed: ++report.drains_failed; break;
      case DrainState::Blocked: ++report.drains_blocked; break;
      case DrainState::Restoring: ++report.drains_restoring; break;
      default: ++report.drains_active; break;
    }

    const std::string label = "drain " + drain::to_string(id) + " (" + record.target().to_string() + ")";
    if (record.state() == DrainState::Drained) {
      const auto outstanding = obligations_.outstanding_protected_dependents_of(record.target());
      if (!outstanding.empty()) {
        report.violations.push_back(label + " is drained but " + std::to_string(outstanding.size()) +
                                    " protected dependenc(ies) still reference the target");
      }
      if (record.remaining_protected_at_completion() != 0) {
        report.violations.push_back(label + " recorded a non-zero protected count at completion");
      }
      if (record.completion_digest() == 0) {
        report.violations.push_back(label + " has no completion digest");
      }
      if (!fence_.is_closed(record.target())) {
        report.violations.push_back(label + " is drained but does not hold a closed admission fence");
      }
      const ResourceNode* node = topology_.resource(record.target().id());
      if (node != nullptr && node->in_service()) {
        report.violations.push_back(label + " is drained but the resource is still in service");
      }
    }

    if (record.state() == DrainState::Cancelled) {
      const FenceEntry* entry = fence_.entry(record.target());
      if (record.restored_at().count() != 0) {
        if (entry == nullptr || entry->generation <= record.generation()) {
          report.violations.push_back(label + " was restored without advancing the admission generation");
        } else if (entry->closed && entry->drain == record.id()) {
          report.violations.push_back(label + " was restored but still owns a closed admission fence");
        }
      } else if (record.generation().value() != 0) {
        // The drain closed admission, so the fence must still be held: a
        // cancelled drain never returns its target to service by itself.
        if (entry == nullptr || !entry->closed) {
          report.violations.push_back(label + " was cancelled after closing admission but the fence is open");
        } else if (entry->generation != record.generation()) {
          report.violations.push_back(label + " was cancelled but the fence generation moved on");
        }
      } else if (entry != nullptr && entry->closed && entry->drain == record.id()) {
        report.violations.push_back(
            label + " was cancelled before closing admission yet holds a closed fence");
      }
    }

    const bool settled = is_settled(record.state());
    if (!settled && record.state() != DrainState::Restoring) {
      const bool expects_fence = holds_fence(record) || is_fence_holding_state(record.state());
      const bool has_fence = fence_.is_closed(record.target());
      if (expects_fence && !has_fence) {
        report.violations.push_back(label + " is past admission closure but its fence is open");
      }
    }
    if (record.state() == DrainState::Restoring) {
      const FenceEntry* entry = fence_.entry(record.target());
      if (entry != nullptr && entry->closed) {
        report.violations.push_back(label + " is restoring but its admission fence is still closed");
      }
    }
  }

  const std::size_t classified = report.drains_active + report.drains_blocked + report.drains_drained +
                                 report.drains_cancelled + report.drains_failed + report.drains_restoring;
  if (classified != report.drains_total) {
    report.violations.push_back("drain accounting does not close: classified " + std::to_string(classified) +
                                " of " + std::to_string(report.drains_total));
  }

  for (const Obligation* obligation : obligations_.all()) {
    if (obligation->blocks_completion()) {
      ++report.obligations_outstanding_protected;
    }
    if (obligation->state() == ObligationState::Retired) {
      ++report.obligations_retired;
    }
    report.obligations_reappeared += obligation->reappearances();
    for (const auto& dependency : obligation->dependencies()) {
      const FenceEntry* entry = fence_.entry(dependency);
      if (entry == nullptr || !entry->closed) {
        continue;
      }
      if (obligation->admission_generation() >= entry->generation) {
        report.violations.push_back(
            obligation->label() + " was admitted at generation " +
            drain::to_string(obligation->admission_generation()) + " but admission for " +
            dependency.to_string() + " closed at generation " + drain::to_string(entry->generation));
      }
    }
  }

  for (const auto& [target, entry] : fence_.entries()) {
    if (!entry.closed) {
      continue;
    }
    ++report.fences_closed;
    const auto it = drains_.find(entry.drain);
    if (it == drains_.end()) {
      report.violations.push_back("closed admission fence for " + target.to_string() +
                                  " references an unknown drain " + drain::to_string(entry.drain));
      continue;
    }
    if (it->second.state() == DrainState::Cancelled && it->second.restored_at().count() != 0) {
      report.violations.push_back("restored drain " + drain::to_string(entry.drain) +
                                  " still holds a closed admission fence for " + target.to_string());
    }
  }

  std::sort(report.violations.begin(), report.violations.end());
  report.violations.erase(std::unique(report.violations.begin(), report.violations.end()),
                          report.violations.end());
  return report;
}

Explanation DrainEngine::explain(DrainId id) const {
  LockGuard guard(mutex_, lock_depth_);
  Explanation explanation;
  const auto it = drains_.find(id);
  if (it == drains_.end()) {
    explanation.summary = "drain " + drain::to_string(id) + " is not known to this engine";
    return explanation;
  }
  explanation.found = true;
  explanation.drain = it->second;
  explanation.blockers = it->second.blockers();
  for (const Obligation* obligation : obligations_.dependents_of(it->second.target())) {
    explanation.dependents.push_back(*obligation);
    if (obligation->blocks_completion()) {
      explanation.outstanding.push_back(*obligation);
    }
  }
  for (const auto& decision : decisions_) {
    if (decision.drain == id) {
      explanation.history.push_back(decision);
    }
  }
  std::string summary;
  summary.append("state=");
  summary.append(drain::to_string(it->second.state()));
  if (it->second.blocked_from().has_value()) {
    summary.append(" resumable-to=");
    summary.append(drain::to_string(*it->second.blocked_from()));
  }
  summary.append(" generation=");
  summary.append(drain::to_string(it->second.generation()));
  summary.append(" dependents=");
  summary.append(std::to_string(explanation.dependents.size()));
  summary.append(" outstanding-protected=");
  summary.append(std::to_string(explanation.outstanding.size()));
  summary.append(" blockers=");
  summary.append(std::to_string(explanation.blockers.size()));
  if (!explanation.blockers.empty()) {
    summary.append(" first-blocker=");
    summary.append(explanation.blockers.front().render());
  }
  explanation.summary = std::move(summary);
  return explanation;
}

JsonValue DrainEngine::snapshot() const {
  LockGuard guard(mutex_, lock_depth_);
  JsonValue value = JsonValue::object();
  value.set("format_version", JsonValue(static_cast<std::uint64_t>(kSnapshotFormatVersion)));
  value.set("incarnation", JsonValue(authority_.incarnation().value()));
  value.set("epoch", JsonValue(authority_.epoch().value()));
  value.set("drain_high_water", JsonValue(drain_high_water_.value()));
  value.set("set_high_water", JsonValue(set_high_water_.value()));
  value.set("exception_high_water", JsonValue(static_cast<std::uint64_t>(exception_high_water_)));
  value.set("policy", policy_.to_json());
  value.set("topology", topology_.to_json());
  value.set("obligations", obligations_.to_json());
  value.set("evidence", evidence_.to_json());
  value.set("authority", authority_.to_json());
  value.set("fence", fence_.to_json());

  JsonValue drains_json = JsonValue::array();
  for (const auto& [id, record] : drains_) {
    (void)id;
    drains_json.array_ref().push_back(record.to_json());
  }
  value.set("drains", std::move(drains_json));

  JsonValue sets_json = JsonValue::array();
  for (const auto& [id, set] : sets_) {
    (void)id;
    sets_json.array_ref().push_back(set.to_json());
  }
  value.set("sets", std::move(sets_json));

  JsonValue exceptions_json = JsonValue::array();
  for (const auto& [id, grants] : exceptions_) {
    for (const auto& grant : grants) {
      (void)id;
      exceptions_json.array_ref().push_back(grant.to_json());
    }
  }
  value.set("exceptions", std::move(exceptions_json));

  JsonValue nonces_json = JsonValue::array();
  for (const auto& [nonce, set] : nonce_sets_) {
    JsonValue entry = JsonValue::object();
    entry.set("nonce", JsonValue(nonce.value()));
    entry.set("set", JsonValue(set.value()));
    nonces_json.array_ref().push_back(std::move(entry));
  }
  value.set("nonces", std::move(nonces_json));

  JsonValue decisions_json = JsonValue::array();
  for (const auto& decision : decisions_) {
    decisions_json.array_ref().push_back(decision.to_json());
  }
  value.set("decisions", std::move(decisions_json));
  return value;
}

void DrainEngine::clear_state_locked() {
  topology_.clear();
  obligations_.clear();
  evidence_.clear();
  authority_.clear();
  fence_.clear();
  drains_.clear();
  sets_.clear();
  exceptions_.clear();
  nonce_sets_.clear();
  decisions_.clear();
  drain_high_water_ = DrainId{};
  set_high_water_ = DrainSetId{};
  exception_high_water_ = 0;
  stats_ = EngineStats{};
}

Status DrainEngine::restore(const JsonValue& snapshot, IncarnationId previous_incarnation, TimePoint now) {
  LockGuard guard(mutex_, lock_depth_);
  if (!snapshot.is_object()) {
    return fail(ErrorCode::PersistenceCorrupt, "snapshot payload is not a json object");
  }
  const std::uint64_t format = snapshot.get_uint("format_version", 0);
  if (format != static_cast<std::uint64_t>(kSnapshotFormatVersion)) {
    return fail(ErrorCode::PersistenceCorrupt, "unsupported snapshot format version");
  }
  const IncarnationId writer{snapshot.get_uint("incarnation", 0)};
  if (!previous_incarnation.is_zero() && writer != previous_incarnation) {
    return fail(ErrorCode::PersistenceCorrupt,
                "snapshot records incarnation " + drain::to_string(writer) + " but the loader reported " +
                    drain::to_string(previous_incarnation));
  }

  DrainPolicy policy = policy_;
  if (const JsonValue* policy_value = snapshot.find("policy"); policy_value != nullptr) {
    auto parsed = DrainPolicy::from_json(*policy_value);
    if (!parsed.ok()) {
      return fail(ErrorCode::PersistenceCorrupt, "snapshot policy rejected: " + parsed.error().message());
    }
    policy = *parsed;
  }

  Topology topology;
  if (const JsonValue* topology_value = snapshot.find("topology"); topology_value != nullptr) {
    Status loaded = topology.load_from_json(*topology_value);
    if (!loaded.ok()) {
      return loaded;
    }
  }

  ObligationTable obligations(policy.max_obligations());
  if (const JsonValue* value = snapshot.find("obligations"); value != nullptr) {
    Status loaded = obligations.load_from_json(*value);
    if (!loaded.ok()) {
      return loaded;
    }
  }

  EvidenceLedger evidence(policy.max_evidence_records());
  if (const JsonValue* value = snapshot.find("evidence"); value != nullptr) {
    Status loaded = evidence.load_from_json(*value);
    if (!loaded.ok()) {
      return loaded;
    }
  }

  AuthorityRegistry authority;
  if (const JsonValue* value = snapshot.find("authority"); value != nullptr) {
    Status loaded = authority.load_from_json(*value);
    if (!loaded.ok()) {
      return loaded;
    }
  }

  AdmissionFence fence;
  if (const JsonValue* value = snapshot.find("fence"); value != nullptr) {
    Status loaded = fence.load_from_json(*value);
    if (!loaded.ok()) {
      return loaded;
    }
  }

  std::map<DrainId, DrainRecord> drains;
  if (const JsonValue* value = snapshot.find("drains"); value != nullptr) {
    if (!value->is_array()) {
      return fail(ErrorCode::PersistenceCorrupt, "snapshot drains payload is not an array");
    }
    if (value->size() > kMaxDrains) {
      return fail(ErrorCode::BoundsExceeded, "snapshot drain count exceeds bound");
    }
    for (const auto& entry : value->as_array()) {
      auto record = DrainRecord::from_json(entry);
      if (!record.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "drain record rejected: " + record.error().message());
      }
      if (!drains.emplace(record->id(), *record).second) {
        return fail(ErrorCode::PersistenceCorrupt, "duplicate drain id in snapshot");
      }
    }
  }

  std::map<DrainSetId, DrainSet> sets;
  if (const JsonValue* value = snapshot.find("sets"); value != nullptr) {
    if (!value->is_array()) {
      return fail(ErrorCode::PersistenceCorrupt, "snapshot sets payload is not an array");
    }
    if (value->size() > kMaxDrainSets) {
      return fail(ErrorCode::BoundsExceeded, "snapshot drain set count exceeds bound");
    }
    for (const auto& entry : value->as_array()) {
      auto set = DrainSet::from_json(entry);
      if (!set.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "drain set rejected: " + set.error().message());
      }
      if (!sets.emplace(set->id, *set).second) {
        return fail(ErrorCode::PersistenceCorrupt, "duplicate drain set id in snapshot");
      }
    }
  }

  std::map<DrainId, std::vector<ExceptionGrant>> exceptions;
  if (const JsonValue* value = snapshot.find("exceptions"); value != nullptr) {
    if (!value->is_array()) {
      return fail(ErrorCode::PersistenceCorrupt, "snapshot exceptions payload is not an array");
    }
    if (value->size() > kMaxDrains * kHardMaxExceptionExtensions) {
      return fail(ErrorCode::BoundsExceeded, "snapshot exception count exceeds bound");
    }
    for (const auto& entry : value->as_array()) {
      auto grant = ExceptionGrant::from_json(entry);
      if (!grant.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "exception grant rejected: " + grant.error().message());
      }
      exceptions[grant->drain].push_back(*grant);
    }
  }

  std::map<RequestNonce, DrainSetId> nonce_sets;
  if (const JsonValue* value = snapshot.find("nonces"); value != nullptr) {
    if (!value->is_array()) {
      return fail(ErrorCode::PersistenceCorrupt, "snapshot nonce payload is not an array");
    }
    if (value->size() > kMaxDrainSets) {
      return fail(ErrorCode::BoundsExceeded, "snapshot nonce count exceeds bound");
    }
    for (const auto& entry : value->as_array()) {
      const RequestNonce nonce{entry.get_uint("nonce")};
      const DrainSetId set{entry.get_uint("set")};
      if (nonce.is_zero()) {
        continue;
      }
      nonce_sets[nonce] = set;
    }
  }

  std::deque<Decision> decisions;
  if (const JsonValue* value = snapshot.find("decisions"); value != nullptr) {
    if (!value->is_array()) {
      return fail(ErrorCode::PersistenceCorrupt, "snapshot decision payload is not an array");
    }
    if (value->size() > kHardMaxDecisionsRetained) {
      return fail(ErrorCode::BoundsExceeded, "snapshot decision count exceeds bound");
    }
    for (const auto& entry : value->as_array()) {
      auto decision = Decision::from_json(entry);
      if (!decision.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "decision record rejected: " + decision.error().message());
      }
      decisions.push_back(*decision);
    }
  }

  DrainId drain_high_water{DrainId{snapshot.get_uint("drain_high_water", 0)}};
  DrainSetId set_high_water{DrainSetId{snapshot.get_uint("set_high_water", 0)}};
  for (const auto& [id, record] : drains) {
    (void)record;
    if (drain_high_water < id) {
      drain_high_water = id;
    }
  }
  for (const auto& [id, set] : sets) {
    (void)set;
    if (set_high_water < id) {
      set_high_water = id;
    }
  }
  const std::uint64_t declared_exceptions = snapshot.get_uint("exception_high_water", 0);
  const auto narrowed_exceptions = narrow_checked<std::uint32_t>(declared_exceptions);
  if (!narrowed_exceptions.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "snapshot exception high-water mark does not fit a 32-bit counter");
  }

  // Conservative recovery. Nothing that could have been produced by a previous
  // incarnation is treated as current.
  evidence.quarantine_all();
  authority.revoke_all(now, "recovered from snapshot; authority never survives a restart");
  for (auto& [id, record] : drains) {
    (void)id;
    const DrainState state = record.state();
    const bool transient = state == DrainState::Validating || state == DrainState::AdmissionClosed ||
                           state == DrainState::Evacuating || state == DrainState::Quiescing ||
                           state == DrainState::Verifying || state == DrainState::Restoring;
    if (!transient) {
      continue;
    }
    Status blocked = record.transition(DrainState::Blocked, now,
                                       "recovered after restart; revalidation required");
    if (!blocked.ok()) {
      return fail(ErrorCode::PersistenceCorrupt,
                  "recovered drain could not be conservatively blocked: " + blocked.error().message());
    }
    std::vector<Blocker> blockers;
    Blocker blocker;
    blocker.code = BlockerCode::RestartRecoveryPending;
    blocker.subject = "drain:" + drain::to_string(record.id());
    blocker.detail = std::string("recovered from state ") + drain::to_string(state) +
                     "; evidence must be re-attested before the drain can continue";
    blocker.since = now;
    blockers.push_back(std::move(blocker));
    record.set_blockers(std::move(blockers));
  }

  policy_ = std::move(policy);
  topology_ = std::move(topology);
  obligations_ = std::move(obligations);
  evidence_ = std::move(evidence);
  authority_ = std::move(authority);
  fence_ = std::move(fence);
  drains_ = std::move(drains);
  sets_ = std::move(sets);
  exceptions_ = std::move(exceptions);
  nonce_sets_ = std::move(nonce_sets);
  decisions_ = std::move(decisions);
  drain_high_water_ = drain_high_water;
  set_high_water_ = set_high_water;
  exception_high_water_ = *narrowed_exceptions;
  stats_ = EngineStats{};

  Decision decision = make_decision("restore-snapshot", DecisionOutcome::Applied, DrainId{}, DrainSetId{},
                                    Generation{}, now);
  decision.inputs.push_back("previous-incarnation=" + drain::to_string(writer));
  decision.inputs.push_back("drains=" + std::to_string(drains_.size()));
  decision.inputs.push_back("obligations=" + std::to_string(obligations_.size()));
  decision.rejected.push_back("trust-recovered-evidence-without-re-attestation");
  decision.rejected.push_back("resume-transient-drain-states-directly");
  record_decision(std::move(decision));
  return ok_status();
}

}  // namespace drain



