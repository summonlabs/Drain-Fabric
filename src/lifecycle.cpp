#include "drain/lifecycle.hpp"

#include <algorithm>
#include <limits>

namespace drain {
namespace {

std::int64_t read_i64(const JsonValue& value, std::string_view key, std::int64_t fallback = 0) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_int(fallback);
}

std::uint64_t read_u64(const JsonValue& value, std::string_view key, std::uint64_t fallback = 0) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_uint(fallback);
}

}  // namespace

const char* to_string(DrainState state) {
  switch (state) {
    case DrainState::Requested: return "requested";
    case DrainState::Validating: return "validating";
    case DrainState::AdmissionClosed: return "admission-closed";
    case DrainState::Evacuating: return "evacuating";
    case DrainState::Quiescing: return "quiescing";
    case DrainState::Verifying: return "verifying";
    case DrainState::Drained: return "drained";
    case DrainState::Blocked: return "blocked";
    case DrainState::Cancelled: return "cancelled";
    case DrainState::Failed: return "failed";
    case DrainState::Restoring: return "restoring";
  }
  return "unknown";
}

std::optional<DrainState> drain_state_from_string(std::string_view text) {
  if (text == "requested") return DrainState::Requested;
  if (text == "validating") return DrainState::Validating;
  if (text == "admission-closed") return DrainState::AdmissionClosed;
  if (text == "evacuating") return DrainState::Evacuating;
  if (text == "quiescing") return DrainState::Quiescing;
  if (text == "verifying") return DrainState::Verifying;
  if (text == "drained") return DrainState::Drained;
  if (text == "blocked") return DrainState::Blocked;
  if (text == "cancelled") return DrainState::Cancelled;
  if (text == "failed") return DrainState::Failed;
  if (text == "restoring") return DrainState::Restoring;
  return std::nullopt;
}

bool is_settled(DrainState state) {
  return state == DrainState::Drained || state == DrainState::Cancelled || state == DrainState::Failed;
}

bool is_active(DrainState state) { return !is_settled(state); }

bool is_legal_drain_transition(DrainState from, DrainState to) {
  if (from == to) {
    return true;
  }
  switch (from) {
    case DrainState::Requested:
      return to == DrainState::Validating || to == DrainState::Blocked || to == DrainState::Cancelled ||
             to == DrainState::Failed;
    case DrainState::Validating:
      return to == DrainState::AdmissionClosed || to == DrainState::Blocked ||
             to == DrainState::Cancelled || to == DrainState::Failed;
    case DrainState::AdmissionClosed:
      return to == DrainState::Evacuating || to == DrainState::Quiescing || to == DrainState::Blocked ||
             to == DrainState::Cancelled || to == DrainState::Failed;
    case DrainState::Evacuating:
      return to == DrainState::Quiescing || to == DrainState::Blocked || to == DrainState::Cancelled ||
             to == DrainState::Failed;
    case DrainState::Quiescing:
      // Quiescing may fall back to Evacuating when a previously quiesced
      // obligation reappears and needs a new reroute.
      return to == DrainState::Verifying || to == DrainState::Evacuating || to == DrainState::Blocked ||
             to == DrainState::Cancelled || to == DrainState::Failed;
    case DrainState::Verifying:
      return to == DrainState::Drained || to == DrainState::Quiescing || to == DrainState::Blocked ||
             to == DrainState::Cancelled || to == DrainState::Failed;
    case DrainState::Blocked:
      return to == DrainState::Requested || to == DrainState::Validating ||
             to == DrainState::AdmissionClosed || to == DrainState::Evacuating ||
             to == DrainState::Quiescing || to == DrainState::Verifying || to == DrainState::Cancelled ||
             to == DrainState::Failed || to == DrainState::Restoring;
    case DrainState::Cancelled:
      return to == DrainState::Restoring || to == DrainState::Failed;
    case DrainState::Failed:
      return to == DrainState::Restoring || to == DrainState::Cancelled;
    case DrainState::Restoring:
      // Restoration settles back into Cancelled: the drain did not happen and
      // the target is back in service under a fresh generation.
      return to == DrainState::Cancelled || to == DrainState::Failed || to == DrainState::Blocked;
    case DrainState::Drained:
      // A drained target may be returned to service, which requires a fresh
      // generation and therefore a full restoration sequence.
      return to == DrainState::Restoring;
  }
  return false;
}

const char* to_string(BlockerCode code) {
  switch (code) {
    case BlockerCode::AdmissionFenceMissing: return "admission-fence-missing";
    case BlockerCode::ProtectedObligationActive: return "protected-obligation-active";
    case BlockerCode::ObligationGraceExpired: return "obligation-grace-expired";
    case BlockerCode::ReleaseEvidenceMissing: return "release-evidence-missing";
    case BlockerCode::ReleaseEvidenceStale: return "release-evidence-stale";
    case BlockerCode::DomainLimitReached: return "domain-limit-reached";
    case BlockerCode::CorrelatedDrainConflict: return "correlated-drain-conflict";
    case BlockerCode::CapacityInsufficient: return "capacity-insufficient";
    case BlockerCode::RedundancyViolation: return "redundancy-violation";
    case BlockerCode::PathDiversityViolation: return "path-diversity-violation";
    case BlockerCode::AlternatePathLost: return "alternate-path-lost";
    case BlockerCode::AuthorityInvalid: return "authority-invalid";
    case BlockerCode::GenerationSuperseded: return "generation-superseded";
    case BlockerCode::EvacuationAttemptsExhausted: return "evacuation-attempts-exhausted";
    case BlockerCode::ObligationReappeared: return "obligation-reappeared";
    case BlockerCode::RestartRecoveryPending: return "restart-recovery-pending";
    case BlockerCode::TopologyInconsistent: return "topology-inconsistent";
    case BlockerCode::PolicyRevisionMismatch: return "policy-revision-mismatch";
    case BlockerCode::SetPredecessorPending: return "set-predecessor-pending";
    case BlockerCode::ObligationFailed: return "obligation-failed";
    case BlockerCode::QuiescenceEvidenceMissing: return "quiescence-evidence-missing";
  }
  return "unknown";
}

JsonValue Blocker::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("code", JsonValue(std::string(drain::to_string(code))));
  value.set("subject", JsonValue(subject));
  value.set("detail", JsonValue(detail));
  value.set("since_ns", JsonValue(static_cast<std::uint64_t>(since.count())));
  value.set("deadline_ns", JsonValue(static_cast<std::uint64_t>(deadline.count())));
  return value;
}

Result<Blocker> Blocker::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "blocker entry is not an object");
  }
  const std::string code_text = value.get_string("code");
  Blocker blocker;
  bool matched = false;
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(BlockerCode::QuiescenceEvidenceMissing); ++raw) {
    const auto candidate = static_cast<BlockerCode>(raw);
    if (code_text == drain::to_string(candidate)) {
      blocker.code = candidate;
      matched = true;
      break;
    }
  }
  if (!matched) {
    return Error(ErrorCode::PersistenceCorrupt, "unknown blocker code in snapshot");
  }
  blocker.subject = value.get_string("subject");
  blocker.detail = value.get_string("detail");
  blocker.since = TimePoint{read_i64(value, "since_ns")};
  blocker.deadline = TimePoint{read_i64(value, "deadline_ns")};
  return blocker;
}

std::string Blocker::render() const {
  std::string out = drain::to_string(code);
  out.append(" subject=");
  out.append(subject);
  out.append(" since=");
  out.append(describe_time(since));
  if (deadline.count() != 0) {
    out.append(" deadline=");
    out.append(describe_time(deadline));
  }
  if (!detail.empty()) {
    out.append(" detail=");
    out.append(detail);
  }
  return out;
}

DrainRecord::DrainRecord(DrainId id, DrainTarget target, Generation generation, AuthorityId authority,
                         TimePoint at)
    : id_(id),
      target_(std::move(target)),
      generation_(generation),
      authority_(authority),
      requested_at_(at),
      state_since_(at) {}

Status DrainRecord::transition(DrainState next, TimePoint at, std::string detail) {
  if (next == state_) {
    return ok_status();
  }
  if (!is_legal_drain_transition(state_, next)) {
    return fail(ErrorCode::IllegalTransition, std::string("drain ") + drain::to_string(id_) + " cannot move from " +
                                                  drain::to_string(state_) + " to " + drain::to_string(next));
  }
  if (revision_.value() == std::numeric_limits<std::uint64_t>::max()) {
    return fail(ErrorCode::BoundsExceeded, std::string("drain ") + drain::to_string(id_) +
                                               " revision space exhausted");
  }
  if (next == DrainState::Blocked) {
    blocked_from_ = state_;
  } else if (state_ == DrainState::Blocked) {
    blocked_from_.reset();
    blockers_.clear();
  }
  state_ = next;
  state_since_ = at;
  revision_ = revision_.next();
  if (next == DrainState::AdmissionClosed) {
    admission_closed_at_ = at;
  }
  if (next == DrainState::Drained || next == DrainState::Cancelled || next == DrainState::Failed) {
    terminal_detail_ = std::move(detail);
  }
  if (next == DrainState::Restoring) {
    terminal_detail_.clear();
  }
  return ok_status();
}

void DrainRecord::set_blockers(std::vector<Blocker> blockers) {
  std::sort(blockers.begin(), blockers.end());
  blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
  blockers_ = std::move(blockers);
}

Status DrainRecord::record_completion(TimePoint at, std::uint32_t remaining_protected, std::uint64_t digest,
                                      EvidenceSeq evidence) {
  if (remaining_protected != 0) {
    return fail(ErrorCode::ProtectedObligationRemains,
                "refusing to complete drain " + drain::to_string(id_) + ": " +
                    std::to_string(remaining_protected) + " protected dependencies remain on " +
                    target_.to_string());
  }
  completed_at_ = at;
  remaining_protected_at_completion_ = remaining_protected;
  completion_digest_ = digest;
  completion_evidence_ = evidence;
  return ok_status();
}

void DrainRecord::note_evacuation_attempt(TimePoint at) {
  last_evacuation_at_ = at;
  ++evacuation_requests_;
}

void DrainRecord::mark_restored(TimePoint at, Generation reopened_generation) {
  restored_at_ = at;
  restore_generation_ = reopened_generation;
}

bool DrainRecord::has_blocker(BlockerCode code) const {
  return std::any_of(blockers_.begin(), blockers_.end(),
                     [code](const Blocker& blocker) { return blocker.code == code; });
}

JsonValue DrainRecord::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id_.value()));
  value.set("set", JsonValue(set_.value()));
  value.set("target", drain::to_json(target_));
  value.set("generation", JsonValue(generation_.value()));
  value.set("restore_generation", JsonValue(restore_generation_.value()));
  value.set("attempt", JsonValue(static_cast<std::uint64_t>(attempt_.value())));
  value.set("revision", JsonValue(revision_.value()));
  value.set("state", JsonValue(std::string(drain::to_string(state_))));
  if (blocked_from_.has_value()) {
    value.set("blocked_from", JsonValue(std::string(drain::to_string(*blocked_from_))));
  }
  value.set("authority", JsonValue(authority_.value()));
  value.set("node", JsonValue(node_.str()));
  value.set("requested_at_ns", JsonValue(static_cast<std::uint64_t>(requested_at_.count())));
  value.set("state_since_ns", JsonValue(static_cast<std::uint64_t>(state_since_.count())));
  value.set("admission_closed_at_ns", JsonValue(static_cast<std::uint64_t>(admission_closed_at_.count())));
  value.set("completed_at_ns", JsonValue(static_cast<std::uint64_t>(completed_at_.count())));
  value.set("restored_at_ns", JsonValue(static_cast<std::uint64_t>(restored_at_.count())));
  value.set("last_evacuation_at_ns", JsonValue(static_cast<std::uint64_t>(last_evacuation_at_.count())));
  value.set("evacuation_requests", JsonValue(static_cast<std::uint64_t>(evacuation_requests_)));
  value.set("remaining_protected_at_completion",
            JsonValue(static_cast<std::uint64_t>(remaining_protected_at_completion_)));
  value.set("completion_digest", JsonValue(completion_digest_));
  value.set("completion_evidence", JsonValue(completion_evidence_.value()));
  value.set("reason", JsonValue(reason_));
  value.set("terminal_detail", JsonValue(terminal_detail_));
  JsonValue blockers = JsonValue::array();
  for (const auto& blocker : blockers_) {
    blockers.array_ref().push_back(blocker.to_json());
  }
  value.set("blockers", std::move(blockers));
  return value;
}

Result<DrainRecord> DrainRecord::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "drain record is not an object");
  }
  auto target = drain_target_from_json(value.find("target") == nullptr ? JsonValue(nullptr)
                                                                      : *value.find("target"));
  if (!target.ok()) {
    return Error(ErrorCode::PersistenceCorrupt, "drain record target rejected: " + target.error().message());
  }
  const auto state = drain_state_from_string(value.get_string("state"));
  if (!state.has_value()) {
    return Error(ErrorCode::PersistenceCorrupt, "drain record has an unknown state");
  }
  DrainRecord record(DrainId{read_u64(value, "id")}, *target, Generation{read_u64(value, "generation")},
                     AuthorityId{read_u64(value, "authority")}, TimePoint{read_i64(value, "requested_at_ns")});
  if (record.id().is_zero()) {
    return Error(ErrorCode::PersistenceCorrupt, "drain record has a reserved zero id");
  }
  record.set_set(DrainSetId{read_u64(value, "set")});
  record.set_generation(Generation{read_u64(value, "generation")});
  record.set_restore_generation(Generation{read_u64(value, "restore_generation")});
  record.set_attempt(Attempt{static_cast<std::uint32_t>(read_u64(value, "attempt"))});
  if (const JsonValue* node = value.find("node"); node != nullptr && node->is_string() && !node->as_string().empty()) {
    const auto parsed = NodeId::parse(node->as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::PersistenceCorrupt, "drain record has a malformed node id");
    }
    record.set_node(*parsed);
  }
  record.set_reason(value.get_string("reason"));
  record.terminal_detail_ = value.get_string("terminal_detail");
  record.state_since_ = TimePoint{read_i64(value, "state_since_ns")};
  record.admission_closed_at_ = TimePoint{read_i64(value, "admission_closed_at_ns")};
  record.completed_at_ = TimePoint{read_i64(value, "completed_at_ns")};
  record.restored_at_ = TimePoint{read_i64(value, "restored_at_ns")};
  record.last_evacuation_at_ = TimePoint{read_i64(value, "last_evacuation_at_ns")};
  const std::uint64_t evacuation_requests = read_u64(value, "evacuation_requests");
  if (evacuation_requests > 1000000ULL) {
    return Error(ErrorCode::BoundsExceeded, "drain record evacuation counter exceeds bound");
  }
  record.evacuation_requests_ = static_cast<std::uint32_t>(evacuation_requests);
  record.remaining_protected_at_completion_ =
      static_cast<std::uint32_t>(read_u64(value, "remaining_protected_at_completion"));
  record.completion_digest_ = read_u64(value, "completion_digest");
  record.completion_evidence_ = EvidenceSeq{read_u64(value, "completion_evidence")};

  const JsonValue* blocked_from = value.find("blocked_from");
  if (blocked_from != nullptr && blocked_from->is_string()) {
    const auto parsed = drain_state_from_string(blocked_from->as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::PersistenceCorrupt, "drain record has an unknown blocked_from state");
    }
    record.blocked_from_ = *parsed;
  } else if (*state == DrainState::Blocked) {
    return Error(ErrorCode::PersistenceCorrupt, "blocked drain record lacks blocked_from");
  }

  const JsonValue* blockers = value.find("blockers");
  if (blockers != nullptr && blockers->is_array()) {
    std::vector<Blocker> parsed_blockers;
    parsed_blockers.reserve(blockers->size());
    for (const auto& entry : blockers->as_array()) {
      auto blocker = Blocker::from_json(entry);
      if (!blocker.ok()) {
        return blocker.error();
      }
      parsed_blockers.push_back(*blocker);
    }
    record.set_blockers(std::move(parsed_blockers));
  }

  if (*state == DrainState::Drained) {
    if (record.remaining_protected_at_completion_ != 0) {
      return Error(ErrorCode::PersistenceCorrupt,
                   "drained record claims outstanding protected dependencies");
    }
    if (record.completion_digest_ == 0) {
      return Error(ErrorCode::PersistenceCorrupt, "drained record lacks a completion digest");
    }
  }
  record.state_ = *state;
  return record;
}

}  // namespace drain
