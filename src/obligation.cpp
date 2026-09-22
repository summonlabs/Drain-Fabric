#include "drain/obligation.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "drain/digest.hpp"

namespace drain {
namespace {

std::uint64_t read_u64(const JsonValue& value, std::string_view key, std::uint64_t fallback = 0) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_uint(fallback);
}

std::int64_t read_i64(const JsonValue& value, std::string_view key, std::int64_t fallback = 0) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_int(fallback);
}

}  // namespace

const char* to_string(ObligationKind kind) {
  switch (kind) {
    case ObligationKind::ActiveFlow: return "active-flow";
    case ObligationKind::Reservation: return "reservation";
    case ObligationKind::WorkloadNetworkContract: return "workload-network-contract";
    case ObligationKind::PathDiversityCommitment: return "path-diversity-commitment";
    case ObligationKind::CapacityGuarantee: return "capacity-guarantee";
    case ObligationKind::Supplied: return "supplied";
  }
  return "unknown";
}

const char* to_string(EvacuationMode mode) {
  switch (mode) {
    case EvacuationMode::None: return "none";
    case EvacuationMode::Reroute: return "reroute";
    case EvacuationMode::Migrate: return "migrate";
    case EvacuationMode::Release: return "release";
    case EvacuationMode::RerouteAndRelease: return "reroute-and-release";
  }
  return "unknown";
}

const char* to_string(ProtectionClass protection) {
  switch (protection) {
    case ProtectionClass::Advisory: return "advisory";
    case ProtectionClass::Protected: return "protected";
  }
  return "unknown";
}

const char* to_string(ObligationState state) {
  switch (state) {
    case ObligationState::Admitted: return "admitted";
    case ObligationState::Evacuating: return "evacuating";
    case ObligationState::Quiescing: return "quiescing";
    case ObligationState::Released: return "released";
    case ObligationState::Retired: return "retired";
    case ObligationState::Failed: return "failed";
  }
  return "unknown";
}

std::optional<ObligationKind> obligation_kind_from_string(std::string_view text) {
  if (text == "active-flow") return ObligationKind::ActiveFlow;
  if (text == "reservation") return ObligationKind::Reservation;
  if (text == "workload-network-contract") return ObligationKind::WorkloadNetworkContract;
  if (text == "path-diversity-commitment") return ObligationKind::PathDiversityCommitment;
  if (text == "capacity-guarantee") return ObligationKind::CapacityGuarantee;
  if (text == "supplied") return ObligationKind::Supplied;
  return std::nullopt;
}

std::optional<EvacuationMode> evacuation_mode_from_string(std::string_view text) {
  if (text == "none") return EvacuationMode::None;
  if (text == "reroute") return EvacuationMode::Reroute;
  if (text == "migrate") return EvacuationMode::Migrate;
  if (text == "release") return EvacuationMode::Release;
  if (text == "reroute-and-release") return EvacuationMode::RerouteAndRelease;
  return std::nullopt;
}

std::optional<ProtectionClass> protection_class_from_string(std::string_view text) {
  if (text == "advisory") return ProtectionClass::Advisory;
  if (text == "protected") return ProtectionClass::Protected;
  return std::nullopt;
}

std::optional<ObligationState> obligation_state_from_string(std::string_view text) {
  if (text == "admitted") return ObligationState::Admitted;
  if (text == "evacuating") return ObligationState::Evacuating;
  if (text == "quiescing") return ObligationState::Quiescing;
  if (text == "released") return ObligationState::Released;
  if (text == "retired") return ObligationState::Retired;
  if (text == "failed") return ObligationState::Failed;
  return std::nullopt;
}

EvacuationMode default_evacuation_mode(ObligationKind kind) {
  switch (kind) {
    case ObligationKind::ActiveFlow: return EvacuationMode::RerouteAndRelease;
    case ObligationKind::Reservation: return EvacuationMode::Release;
    case ObligationKind::WorkloadNetworkContract: return EvacuationMode::Migrate;
    case ObligationKind::PathDiversityCommitment: return EvacuationMode::Reroute;
    case ObligationKind::CapacityGuarantee: return EvacuationMode::Release;
    case ObligationKind::Supplied: return EvacuationMode::RerouteAndRelease;
  }
  return EvacuationMode::RerouteAndRelease;
}

namespace {

/// Legal obligation state transitions. Retired -> Admitted models an
/// obligation that reappears after it was accounted for as gone; the engine is
/// required to re-verify rather than trust its previous bookkeeping.
bool is_legal_obligation_transition(ObligationState from, ObligationState to) {
  if (from == to) {
    return true;
  }
  switch (from) {
    case ObligationState::Admitted:
      return to == ObligationState::Evacuating || to == ObligationState::Quiescing ||
             to == ObligationState::Released || to == ObligationState::Retired ||
             to == ObligationState::Failed;
    case ObligationState::Evacuating:
      return to == ObligationState::Quiescing || to == ObligationState::Released ||
             to == ObligationState::Retired || to == ObligationState::Failed;
    case ObligationState::Quiescing:
      return to == ObligationState::Released || to == ObligationState::Retired ||
             to == ObligationState::Failed || to == ObligationState::Admitted;
    case ObligationState::Released:
      return to == ObligationState::Retired || to == ObligationState::Failed ||
             to == ObligationState::Admitted || to == ObligationState::Quiescing;
    case ObligationState::Retired:
      return to == ObligationState::Admitted;
    case ObligationState::Failed:
      return to == ObligationState::Admitted || to == ObligationState::Evacuating ||
             to == ObligationState::Quiescing;
  }
  return false;
}

}  // namespace

Obligation::Obligation(ObligationKind kind, HolderId holder, std::vector<DrainTarget> dependencies)
    : kind_(kind),
      holder_(std::move(holder)),
      mode_(default_evacuation_mode(kind)),
      dependencies_(std::move(dependencies)) {
  normalize_dependencies();
}

void Obligation::normalize_dependencies() {
  std::sort(dependencies_.begin(), dependencies_.end(), target_order_less);
  dependencies_.erase(std::unique(dependencies_.begin(), dependencies_.end()), dependencies_.end());
}

bool Obligation::depends_on(const DrainTarget& target) const {
  return std::binary_search(dependencies_.begin(), dependencies_.end(), target, target_order_less);
}

bool Obligation::depends_on_resource(const ResourceId& id) const {
  return std::any_of(dependencies_.begin(), dependencies_.end(),
                     [&id](const DrainTarget& target) { return target.id() == id; });
}

std::string Obligation::label() const {
  std::string out = "obligation:";
  out.append(drain::to_string(id_));
  out.push_back('/');
  out.append(drain::to_string(kind_));
  out.append(" holder=");
  out.append(holder_.str());
  return out;
}

Status Obligation::apply_state(Revision expected_revision, ObligationState next, EvidenceSeq evidence,
                               TimePoint at) {
  if (expected_revision != revision_) {
    return fail(ErrorCode::StaleRevision,
                "obligation revision mismatch on " + label() + ": expected " +
                    drain::to_string(expected_revision) + " have " + drain::to_string(revision_));
  }
  if (next == state_) {
    // A repeat that carries no new evidence is an idempotent replay and is
    // accepted without mutating anything. A repeat that carries a *newer*
    // evidence record is a re-observation of the same state -- for example a
    // fresh release proof after the previous one went stale -- and replaces the
    // stored proof, bumping the revision so that older replays stay fenced.
    if (evidence.is_zero() || evidence == release_evidence_) {
      return ok_status();
    }
    if (state_ == ObligationState::Quiescing || state_ == ObligationState::Released ||
        state_ == ObligationState::Retired) {
      if (revision_.value() == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::BoundsExceeded, label() + " revision space exhausted");
      }
      release_evidence_ = evidence;
      cleared_at_ = at;
      revision_ = revision_.next();
    }
    return ok_status();
  }
  if (!is_legal_obligation_transition(state_, next)) {
    return fail(ErrorCode::IllegalTransition, label() + " cannot move from " + drain::to_string(state_) +
                                                  " to " + drain::to_string(next));
  }
  if (revision_.value() == std::numeric_limits<std::uint64_t>::max()) {
    return fail(ErrorCode::BoundsExceeded, label() + " revision space exhausted");
  }
  const bool reappearance = next == ObligationState::Admitted && state_ != ObligationState::Admitted;
  state_ = next;
  revision_ = revision_.next();
  if (next == ObligationState::Retired || next == ObligationState::Released) {
    release_evidence_ = evidence;
    cleared_at_ = at;
  }
  if (reappearance) {
    release_evidence_ = EvidenceSeq{};
    cleared_at_ = TimePoint{0};
    ++reappearances_;
  }
  return ok_status();
}

JsonValue Obligation::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id_.value()));
  value.set("kind", JsonValue(std::string(drain::to_string(kind_))));
  value.set("protection", JsonValue(std::string(drain::to_string(protection_))));
  value.set("holder", JsonValue(holder_.str()));
  value.set("service", JsonValue(service_.str()));
  value.set("group", JsonValue(group_.str()));
  value.set("mode", JsonValue(std::string(drain::to_string(mode_))));
  JsonValue deps = JsonValue::array();
  for (const auto& dep : dependencies_) {
    deps.array_ref().push_back(drain::to_json(dep));
  }
  value.set("dependencies", std::move(deps));
  value.set("capacity", JsonValue(capacity_.value()));
  value.set("grace_ns", JsonValue(static_cast<std::uint64_t>(grace_.count())));
  value.set("attempt", JsonValue(static_cast<std::uint64_t>(attempt_.value())));
  value.set("admitted_at_ns", JsonValue(static_cast<std::uint64_t>(admitted_at_.count())));
  value.set("admission_generation", JsonValue(admission_generation_.value()));
  value.set("state", JsonValue(std::string(drain::to_string(state_))));
  value.set("revision", JsonValue(revision_.value()));
  value.set("release_evidence", JsonValue(release_evidence_.value()));
  value.set("cleared_at_ns", JsonValue(static_cast<std::uint64_t>(cleared_at_.count())));
  value.set("reappearances", JsonValue(static_cast<std::uint64_t>(reappearances_)));
  value.set("note", JsonValue(note_));
  return value;
}

Result<Obligation> Obligation::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ProtocolError, "obligation must be a json object");
  }
  const auto kind = obligation_kind_from_string(value.get_string("kind"));
  if (!kind.has_value()) {
    return Error(ErrorCode::ProtocolError, "unknown obligation kind");
  }
  const auto holder = HolderId::parse(value.get_string("holder"));
  if (!holder.has_value()) {
    return Error(ErrorCode::MalformedIdentity, "malformed obligation holder");
  }
  const JsonValue* deps = value.find("dependencies");
  if (deps == nullptr || !deps->is_array()) {
    return Error(ErrorCode::ProtocolError, "obligation dependencies must be an array");
  }
  if (deps->size() > kMaxDependenciesPerObligation) {
    return Error(ErrorCode::BoundsExceeded, "obligation dependency count exceeds bound");
  }
  if (deps->size() == 0) {
    return Error(ErrorCode::ProtocolError, "obligation must depend on at least one target");
  }
  std::vector<DrainTarget> dependencies;
  dependencies.reserve(deps->size());
  for (const auto& entry : deps->as_array()) {
    auto target = drain_target_from_json(entry);
    if (!target.ok()) {
      return target.error();
    }
    dependencies.push_back(*target);
  }

  Obligation obligation(*kind, *holder, std::move(dependencies));

  const auto protection = protection_class_from_string(value.get_string("protection", "protected"));
  if (!protection.has_value()) {
    return Error(ErrorCode::ProtocolError, "unknown obligation protection class");
  }
  obligation.set_protection(*protection);

  const auto mode = evacuation_mode_from_string(value.get_string("mode"));
  if (!mode.has_value()) {
    return Error(ErrorCode::ProtocolError, "unknown obligation evacuation mode");
  }
  obligation.set_evacuation_mode(*mode);

  if (const JsonValue* service = value.find("service"); service != nullptr && service->is_string() &&
                                                                 !service->as_string().empty()) {
    const auto parsed = ServiceId::parse(service->as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "malformed obligation service id");
    }
    obligation.set_service(*parsed);
  }
  if (const JsonValue* group = value.find("group"); group != nullptr && group->is_string() &&
                                                               !group->as_string().empty()) {
    const auto parsed = GroupId::parse(group->as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "malformed obligation group id");
    }
    obligation.set_group(*parsed);
  }

  obligation.set_id(ObligationId{read_u64(value, "id")});
  obligation.set_capacity(CapacityUnits::from(read_u64(value, "capacity")));
  obligation.set_grace(Duration{read_i64(value, "grace_ns")});
  obligation.set_attempt(Attempt{static_cast<std::uint32_t>(read_u64(value, "attempt"))});
  obligation.set_admitted_at(TimePoint{read_i64(value, "admitted_at_ns")});
  obligation.set_admission_generation(Generation{read_u64(value, "admission_generation")});

  const auto state = obligation_state_from_string(value.get_string("state"));
  if (!state.has_value()) {
    return Error(ErrorCode::ProtocolError, "unknown obligation state");
  }
  const std::uint64_t revision = read_u64(value, "revision", 1);
  if (revision == 0) {
    return Error(ErrorCode::ProtocolError, "obligation revision must be non-zero");
  }
  // A stored obligation must be reachable from Admitted through the legality
  // table. Intermediate steps are not replayed -- only the transition is
  // validated -- but an unreachable state is rejected outright.
  if (!is_legal_obligation_transition(ObligationState::Admitted, *state)) {
    return Error(ErrorCode::PersistenceCorrupt, "obligation state is unreachable from admitted");
  }
  const std::uint64_t reappearances = read_u64(value, "reappearances");
  if (reappearances > kMaxObligationReappearances) {
    return Error(ErrorCode::BoundsExceeded, "obligation reappearance counter exceeds bound");
  }
  obligation.state_ = *state;
  obligation.revision_ = Revision{revision};
  obligation.release_evidence_ = EvidenceSeq{read_u64(value, "release_evidence")};
  obligation.cleared_at_ = TimePoint{read_i64(value, "cleared_at_ns")};
  obligation.reappearances_ = static_cast<std::uint32_t>(reappearances);
  obligation.set_note(value.get_string("note"));
  return obligation;
}

Result<Obligation> ObligationTable::insert(Obligation draft) {
  if (obligations_.size() >= max_obligations_) {
    return Error(ErrorCode::BoundsExceeded, "obligation table is at capacity");
  }
  const auto next = increment_checked(high_water_.value());
  if (!next.has_value()) {
    return Error(ErrorCode::BoundsExceeded, "obligation id space exhausted");
  }
  high_water_ = ObligationId{*next};
  draft.set_id(high_water_);
  if (!obligations_.emplace(high_water_, draft).second) {
    return Error(ErrorCode::DuplicateIdentity, "obligation id already present");
  }
  return draft;
}

Result<Obligation> ObligationTable::insert_with_id(Obligation draft) {
  if (obligations_.size() >= max_obligations_) {
    return Error(ErrorCode::BoundsExceeded, "obligation table is at capacity");
  }
  if (draft.id().is_zero()) {
    return Error(ErrorCode::DuplicateIdentity, "obligation id zero is reserved");
  }
  if (!obligations_.emplace(draft.id(), draft).second) {
    return Error(ErrorCode::DuplicateIdentity, "duplicate obligation id in snapshot");
  }
  if (high_water_ < draft.id()) {
    high_water_ = draft.id();
  }
  return draft;
}

const Obligation* ObligationTable::find(ObligationId id) const {
  const auto it = obligations_.find(id);
  return it == obligations_.end() ? nullptr : &it->second;
}

Obligation* ObligationTable::find_mutable(ObligationId id) {
  const auto it = obligations_.find(id);
  return it == obligations_.end() ? nullptr : &it->second;
}

std::vector<const Obligation*> ObligationTable::dependents_of(const DrainTarget& target) const {
  std::vector<const Obligation*> out;
  for (const auto& [id, obligation] : obligations_) {
    (void)id;
    if (obligation.depends_on(target)) {
      out.push_back(&obligation);
    }
  }
  return out;
}

std::vector<const Obligation*> ObligationTable::protected_dependents_of(const DrainTarget& target) const {
  std::vector<const Obligation*> out;
  for (const auto& [id, obligation] : obligations_) {
    (void)id;
    if (obligation.protection() == ProtectionClass::Protected && obligation.depends_on(target)) {
      out.push_back(&obligation);
    }
  }
  return out;
}

std::vector<const Obligation*> ObligationTable::outstanding_protected_dependents_of(
    const DrainTarget& target) const {
  std::vector<const Obligation*> out;
  for (const auto& [id, obligation] : obligations_) {
    (void)id;
    if (obligation.blocks_completion() && obligation.depends_on(target)) {
      out.push_back(&obligation);
    }
  }
  return out;
}

std::uint64_t ObligationTable::digest() const {
  Digest64 digest;
  digest.update_u64(high_water_.value());
  for (const auto& [id, obligation] : obligations_) {
    digest.update_u64(id.value());
    digest.update_tagged("kind", drain::to_string(obligation.kind()));
    digest.update_tagged("protection", drain::to_string(obligation.protection()));
    digest.update_tagged("state", drain::to_string(obligation.state()));
    digest.update_u64(obligation.revision().value());
    digest.update_u64(obligation.reappearances());
    digest.update_tagged("holder", obligation.holder().str());
    for (const auto& dep : obligation.dependencies()) {
      digest.update_tagged("dep", dep.to_string());
    }
  }
  return digest.value();
}

JsonValue ObligationTable::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("high_water", JsonValue(high_water_.value()));
  JsonValue items = JsonValue::array();
  for (const auto& [id, obligation] : obligations_) {
    (void)id;
    items.array_ref().push_back(obligation.to_json());
  }
  value.set("obligations", std::move(items));
  return value;
}

Status ObligationTable::load_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return fail(ErrorCode::PersistenceCorrupt, "obligation table payload is not an object");
  }
  const JsonValue* items = value.find("obligations");
  if (items == nullptr || !items->is_array()) {
    return fail(ErrorCode::PersistenceCorrupt, "obligation table payload lacks an obligations array");
  }
  if (items->size() > max_obligations_) {
    return fail(ErrorCode::BoundsExceeded, "snapshot obligation count exceeds the configured bound");
  }
  std::map<ObligationId, Obligation> loaded;
  ObligationId high_water{};
  for (const auto& entry : items->as_array()) {
    auto obligation = Obligation::from_json(entry);
    if (!obligation.ok()) {
      return fail(ErrorCode::PersistenceCorrupt,
                  "obligation entry rejected: " + obligation.error().message());
    }
    if (obligation->id().is_zero()) {
      return fail(ErrorCode::PersistenceCorrupt, "obligation entry has a reserved zero id");
    }
    if (!loaded.emplace(obligation->id(), *obligation).second) {
      return fail(ErrorCode::PersistenceCorrupt, "duplicate obligation id in snapshot");
    }
    if (high_water < obligation->id()) {
      high_water = obligation->id();
    }
  }
  const std::uint64_t declared = read_u64(value, "high_water");
  if (declared < high_water.value()) {
    return fail(ErrorCode::PersistenceCorrupt, "obligation high-water mark is behind the stored ids");
  }
  obligations_ = std::move(loaded);
  high_water_ = ObligationId{declared};
  return ok_status();
}

}  // namespace drain
