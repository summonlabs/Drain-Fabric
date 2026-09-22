#pragma once

// Drain Fabric -- explicit obligation model.
//
// An obligation is any dependency that a resource currently carries: an active
// flow, a reservation, a workload network contract, a path-diversity
// commitment, a capacity guarantee, or a caller-supplied dependency. The drain
// lifecycle is only allowed to complete when every *protected* obligation that
// depends on the drained resource has been retired under the authoritative
// snapshot. Nothing in this file decides how an obligation is evacuated --
// evacuation is requested from adjacent runtimes (see evacuation.hpp).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "drain/clock.hpp"
#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"

namespace drain {

/// Bound on how many resources a single obligation may depend on. Enforced
/// wherever obligations are constructed, including snapshot recovery.
inline constexpr std::size_t kMaxDependenciesPerObligation = 64;
/// Bound on the reappearance counter, so that a corrupt snapshot cannot store an
/// absurd value.
inline constexpr std::uint32_t kMaxObligationReappearances = 1000000u;

/// The kind of dependency a resource carries. The kind determines the default
/// evacuation mode; callers may override it for supplied obligations.
enum class ObligationKind : std::uint8_t {
  ActiveFlow = 0,
  Reservation = 1,
  WorkloadNetworkContract = 2,
  PathDiversityCommitment = 3,
  CapacityGuarantee = 4,
  Supplied = 5,
};

/// How the obligation must be cleared before removal. Drain Fabric *requests*
/// these operations; it does not implement them.
enum class EvacuationMode : std::uint8_t {
  None = 0,
  Reroute = 1,
  Migrate = 2,
  Release = 3,
  RerouteAndRelease = 4,
};

/// Advisory obligations are recorded and reported but do not block completion.
/// Protected obligations block completion until they are retired.
enum class ProtectionClass : std::uint8_t { Advisory = 0, Protected = 1 };

/// Obligation lifecycle. Retired is the only state that satisfies a drain.
enum class ObligationState : std::uint8_t {
  Admitted = 0,
  Evacuating = 1,
  Quiescing = 2,
  Released = 3,
  Retired = 4,
  Failed = 5,
};

DRAIN_API const char* to_string(ObligationKind kind);
DRAIN_API const char* to_string(EvacuationMode mode);
DRAIN_API const char* to_string(ProtectionClass protection);
DRAIN_API const char* to_string(ObligationState state);

DRAIN_API std::optional<ObligationKind> obligation_kind_from_string(std::string_view text);
DRAIN_API std::optional<EvacuationMode> evacuation_mode_from_string(std::string_view text);
DRAIN_API std::optional<ProtectionClass> protection_class_from_string(std::string_view text);
DRAIN_API std::optional<ObligationState> obligation_state_from_string(std::string_view text);

/// Default evacuation mode implied by the obligation kind.
DRAIN_API EvacuationMode default_evacuation_mode(ObligationKind kind);

/// A single dependency carried by one or more resources.
class Obligation {
 public:
  Obligation() = default;
  Obligation(ObligationKind kind, HolderId holder, std::vector<DrainTarget> dependencies);

  ObligationId id() const noexcept { return id_; }
  void set_id(ObligationId id) noexcept { id_ = id; }

  ObligationKind kind() const noexcept { return kind_; }

  ProtectionClass protection() const noexcept { return protection_; }
  void set_protection(ProtectionClass protection) noexcept { protection_ = protection; }

  const HolderId& holder() const noexcept { return holder_; }

  const ServiceId& service() const noexcept { return service_; }
  void set_service(ServiceId service) noexcept { service_ = std::move(service); }

  const GroupId& group() const noexcept { return group_; }
  void set_group(GroupId group) noexcept { group_ = std::move(group); }

  EvacuationMode evacuation_mode() const noexcept { return mode_; }
  void set_evacuation_mode(EvacuationMode mode) noexcept { mode_ = mode; }

  CapacityUnits capacity() const noexcept { return capacity_; }
  void set_capacity(CapacityUnits capacity) noexcept { capacity_ = capacity; }

  /// Policy-defined grace deadline relative to admission. A zero duration means
  /// "use the policy default for this kind".
  Duration grace() const noexcept { return grace_; }
  void set_grace(Duration grace) noexcept { grace_ = grace; }

  Attempt attempt() const noexcept { return attempt_; }
  void set_attempt(Attempt attempt) noexcept { attempt_ = attempt; }

  TimePoint admitted_at() const noexcept { return admitted_at_; }
  void set_admitted_at(TimePoint at) noexcept { admitted_at_ = at; }

  Generation admission_generation() const noexcept { return admission_generation_; }
  void set_admission_generation(Generation generation) noexcept { admission_generation_ = generation; }

  ObligationState state() const noexcept { return state_; }
  Revision revision() const noexcept { return revision_; }

  const std::vector<DrainTarget>& dependencies() const noexcept { return dependencies_; }

  /// Set when the obligation is retired or released; identifies the evidence
  /// record that proved it.
  EvidenceSeq release_evidence() const noexcept { return release_evidence_; }
  TimePoint cleared_at() const noexcept { return cleared_at_; }

  /// Number of times this obligation reappeared after being released or
  /// retired. A non-zero value means an adjacent runtime re-bound a dependency
  /// that Drain Fabric had already accounted for; every drain that observes a
  /// reappearance must re-verify before completing.
  std::uint32_t reappearances() const noexcept { return reappearances_; }

  const std::string& note() const noexcept { return note_; }
  void set_note(std::string note) noexcept { note_ = std::move(note); }

  bool depends_on(const DrainTarget& target) const;
  bool depends_on_resource(const ResourceId& id) const;

  /// True when this obligation prevents a drain from being declared complete.
  bool blocks_completion() const noexcept {
    return protection_ == ProtectionClass::Protected && state_ != ObligationState::Retired;
  }

  /// True when this obligation still has to be moved or released.
  bool needs_evacuation() const noexcept {
    return state_ == ObligationState::Admitted || state_ == ObligationState::Evacuating ||
           state_ == ObligationState::Quiescing || state_ == ObligationState::Released;
  }

  /// Canonical one-line identity used in explanations.
  std::string label() const;

  /// Applies a state change. The expected revision fences duplicate or
  /// reordered callbacks: a callback that does not carry the current revision
  /// is rejected without mutating state.
  Status apply_state(Revision expected_revision, ObligationState next, EvidenceSeq evidence, TimePoint at);

  JsonValue to_json() const;
  static Result<Obligation> from_json(const JsonValue& value);

 private:
  void normalize_dependencies();

  ObligationId id_{};
  ObligationKind kind_{ObligationKind::ActiveFlow};
  ProtectionClass protection_{ProtectionClass::Protected};
  HolderId holder_{};
  ServiceId service_{};
  GroupId group_{};
  EvacuationMode mode_{EvacuationMode::RerouteAndRelease};
  std::vector<DrainTarget> dependencies_{};
  CapacityUnits capacity_{};
  Duration grace_{0};
  Attempt attempt_{};
  TimePoint admitted_at_{0};
  Generation admission_generation_{};
  ObligationState state_{ObligationState::Admitted};
  Revision revision_{Revision{1}};
  EvidenceSeq release_evidence_{};
  TimePoint cleared_at_{0};
  std::uint32_t reappearances_{0};
  std::string note_{};
};

/// Ordered, bounded store of obligations. Iteration order is by obligation id
/// so that every explanation produced from this table is deterministic.
///
/// Obligations are never erased: a retired obligation remains visible as
/// historical evidence, which is what makes accounting closure checkable.
class ObligationTable {
 public:
  explicit ObligationTable(std::size_t max_obligations = 100000) : max_obligations_(max_obligations) {}

  /// Inserts a draft, assigning the next obligation id. The caller-provided id
  /// on the draft is ignored. Returns a copy of the stored obligation.
  Result<Obligation> insert(Obligation draft);

  /// Inserts with an explicit id (used only by snapshot recovery). Rejects
  /// duplicate ids.
  Result<Obligation> insert_with_id(Obligation draft);

  const Obligation* find(ObligationId id) const;
  Obligation* find_mutable(ObligationId id);

  std::vector<const Obligation*> dependents_of(const DrainTarget& target) const;
  std::vector<const Obligation*> protected_dependents_of(const DrainTarget& target) const;

  /// Protected dependents that are not yet retired -- the drain completion gate.
  std::vector<const Obligation*> outstanding_protected_dependents_of(const DrainTarget& target) const;

  /// Every obligation, in id order. Used by audits and explanations.
  std::vector<const Obligation*> all() const {
    std::vector<const Obligation*> out;
    out.reserve(obligations_.size());
    for (const auto& [id, obligation] : obligations_) {
      (void)id;
      out.push_back(&obligation);
    }
    return out;
  }

  std::size_t size() const noexcept { return obligations_.size(); }
  std::size_t max_obligations() const noexcept { return max_obligations_; }
  void set_max_obligations(std::size_t value) noexcept { max_obligations_ = value; }
  ObligationId high_water() const noexcept { return high_water_; }

  /// Deterministic content digest over every obligation identity and state.
  std::uint64_t digest() const;

  JsonValue to_json() const;
  Status load_from_json(const JsonValue& value);

  void clear() {
    obligations_.clear();
    high_water_ = ObligationId{};
  }

 private:
  std::map<ObligationId, Obligation> obligations_{};
  std::size_t max_obligations_{100000};
  ObligationId high_water_{};
};

}  // namespace drain
