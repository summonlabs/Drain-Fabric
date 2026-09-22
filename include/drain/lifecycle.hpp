#pragma once

// Drain Fabric -- drain lifecycle state machine and blocker taxonomy.
//
// The lifecycle is a directed graph with an explicit legality table. Every
// transition is recorded with the generation, authority, and evidence that
// governed it, so a drain history is reproducible from persisted state.
//
// The lifecycle deliberately contains no wall-clock timeout. Policy deadlines
// produce *blockers* and exception requirements; they never silently fail or
// complete a drain.

#include <cstdint>
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

/// Lifecycle position of a single drain.
///
/// Requested -> Validating -> AdmissionClosed -> Evacuating -> Quiescing ->
/// Verifying -> Drained, with Blocked / Cancelled / Failed / Restoring as the
/// exceptional states. Evacuating and Quiescing are distinct because they are
/// driven by different adjacent-runtime operations: Evacuating means at least
/// one obligation still needs a reroute or migration, Quiescing means every
/// obligation has been moved and only release/quiescence confirmation remains.
enum class DrainState : std::uint8_t {
  Requested = 0,
  Validating = 1,
  AdmissionClosed = 2,
  Evacuating = 3,
  Quiescing = 4,
  Verifying = 5,
  Drained = 6,
  Blocked = 7,
  Cancelled = 8,
  Failed = 9,
  Restoring = 10,
};

DRAIN_API const char* to_string(DrainState state);
DRAIN_API std::optional<DrainState> drain_state_from_string(std::string_view text);

/// True when the drain has stopped making progress on its own. Restoring is not
/// settled: it is the state in which admission is reopened.
DRAIN_API bool is_settled(DrainState state);

/// True when the drain may still reach Drained.
DRAIN_API bool is_active(DrainState state);

/// Legal transition check. Self-transitions are legal and mean "no change".
DRAIN_API bool is_legal_drain_transition(DrainState from, DrainState to);

/// Why a drain is not progressing. Blockers are the deterministic explanation
/// surface: they name the obligation, path, capacity pool, or evidence record
/// that prevents the next transition.
enum class BlockerCode : std::uint8_t {
  AdmissionFenceMissing = 0,
  ProtectedObligationActive = 1,
  ObligationGraceExpired = 2,
  ReleaseEvidenceMissing = 3,
  ReleaseEvidenceStale = 4,
  DomainLimitReached = 5,
  CorrelatedDrainConflict = 6,
  CapacityInsufficient = 7,
  RedundancyViolation = 8,
  PathDiversityViolation = 9,
  AlternatePathLost = 10,
  AuthorityInvalid = 11,
  GenerationSuperseded = 12,
  EvacuationAttemptsExhausted = 13,
  ObligationReappeared = 14,
  RestartRecoveryPending = 15,
  TopologyInconsistent = 16,
  PolicyRevisionMismatch = 17,
  SetPredecessorPending = 18,
  ObligationFailed = 19,
  QuiescenceEvidenceMissing = 20,
};

DRAIN_API const char* to_string(BlockerCode code);

struct Blocker {
  BlockerCode code{BlockerCode::ProtectedObligationActive};
  /// Canonical identity of the thing that blocks: an obligation label, a
  /// target, a path id, a capacity pool, or an evidence key.
  std::string subject{};
  /// Human-readable detail. Deterministic: built only from persisted state.
  std::string detail{};
  /// When the condition was first observed.
  TimePoint since{0};
  /// Policy deadline attached to the condition, or zero when none applies.
  TimePoint deadline{0};

  friend bool operator==(const Blocker& a, const Blocker& b) noexcept {
    return a.code == b.code && a.subject == b.subject;
  }
  friend bool operator<(const Blocker& a, const Blocker& b) noexcept {
    if (a.code != b.code) {
      return static_cast<std::uint8_t>(a.code) < static_cast<std::uint8_t>(b.code);
    }
    return a.subject < b.subject;
  }

  JsonValue to_json() const;
  static Result<Blocker> from_json(const JsonValue& value);

  /// Canonical single-line rendering used by the CLI and by the determinism
  /// tests.
  std::string render() const;
};

/// One drain of one target. The drain generation is the authority generation
/// under which admission for this target was closed; evidence and obligation
/// reports that carry a different generation are rejected.
class DrainRecord {
 public:
  DrainRecord() = default;
  DrainRecord(DrainId id, DrainTarget target, Generation generation, AuthorityId authority, TimePoint at);

  DrainId id() const noexcept { return id_; }
  const DrainSetId& set() const noexcept { return set_; }
  void set_set(DrainSetId set) noexcept { set_ = set; }

  const DrainTarget& target() const noexcept { return target_; }

  Generation generation() const noexcept { return generation_; }
  void set_generation(Generation generation) noexcept { generation_ = generation; }

  /// Generation under which admission was reopened during restoration. Distinct
  /// from the closed generation so that stale authority from the closed
  /// generation cannot be resurrected.
  Generation restore_generation() const noexcept { return restore_generation_; }
  void set_restore_generation(Generation generation) noexcept { restore_generation_ = generation; }

  Attempt attempt() const noexcept { return attempt_; }
  void set_attempt(Attempt attempt) noexcept { attempt_ = attempt; }

  Revision revision() const noexcept { return revision_; }

  DrainState state() const noexcept { return state_; }

  /// State that Blocked is holding, when the drain is blocked.
  std::optional<DrainState> blocked_from() const noexcept { return blocked_from_; }

  AuthorityId authority() const noexcept { return authority_; }
  void set_authority(AuthorityId authority) noexcept { authority_ = authority; }

  const NodeId& node() const noexcept { return node_; }
  void set_node(NodeId node) noexcept { node_ = std::move(node); }

  TimePoint requested_at() const noexcept { return requested_at_; }
  TimePoint state_since() const noexcept { return state_since_; }
  TimePoint admission_closed_at() const noexcept { return admission_closed_at_; }
  TimePoint completed_at() const noexcept { return completed_at_; }
  TimePoint restored_at() const noexcept { return restored_at_; }
  TimePoint last_evacuation_at() const noexcept { return last_evacuation_at_; }

  std::uint32_t evacuation_requests() const noexcept { return evacuation_requests_; }

  /// Protected dependencies counted under the authoritative snapshot at the
  /// moment the drain completed. Must be zero for every Drained record.
  std::uint32_t remaining_protected_at_completion() const noexcept {
    return remaining_protected_at_completion_;
  }

  /// Digest of the authoritative snapshot that proved completion.
  std::uint64_t completion_digest() const noexcept { return completion_digest_; }
  EvidenceSeq completion_evidence() const noexcept { return completion_evidence_; }

  const std::string& reason() const noexcept { return reason_; }
  void set_reason(std::string reason) noexcept { reason_ = std::move(reason); }

  const std::string& terminal_detail() const noexcept { return terminal_detail_; }

  const std::vector<Blocker>& blockers() const noexcept { return blockers_; }

  /// Applies a state change, validating the transition and bumping the revision.
  Status transition(DrainState next, TimePoint at, std::string detail);

  /// Replaces the blocker set. Blockers are normalised (sorted, de-duplicated)
  /// so that explanation output is deterministic regardless of discovery order.
  void set_blockers(std::vector<Blocker> blockers);

  /// Records the completion proof. Rejected when protected dependencies remain:
  /// a drain is never declared complete with outstanding protected work.
  Status record_completion(TimePoint at, std::uint32_t remaining_protected, std::uint64_t digest,
                           EvidenceSeq evidence);

  void note_evacuation_attempt(TimePoint at);

  /// Records that restoration finished: admission for this target was reopened
  /// under a fresh generation.
  void mark_restored(TimePoint at, Generation reopened_generation);

  /// Clears the automatic evacuation retry budget. Used when an operator grants
  /// a policy exception, which is the only sanctioned way to obtain more
  /// automatic retries.
  void reset_evacuation_attempts() noexcept { evacuation_requests_ = 0; }

  bool has_blocker(BlockerCode code) const;

  JsonValue to_json() const;
  static Result<DrainRecord> from_json(const JsonValue& value);

 private:
  DrainId id_{};
  DrainSetId set_{};
  DrainTarget target_{};
  Generation generation_{};
  Generation restore_generation_{};
  Attempt attempt_{};
  Revision revision_{Revision{1}};
  DrainState state_{DrainState::Requested};
  std::optional<DrainState> blocked_from_{};
  AuthorityId authority_{};
  NodeId node_{};
  TimePoint requested_at_{0};
  TimePoint state_since_{0};
  TimePoint admission_closed_at_{0};
  TimePoint completed_at_{0};
  TimePoint restored_at_{0};
  TimePoint last_evacuation_at_{0};
  std::uint32_t evacuation_requests_{0};
  std::uint32_t remaining_protected_at_completion_{0};
  std::uint64_t completion_digest_{0};
  EvidenceSeq completion_evidence_{};
  std::string reason_{};
  std::string terminal_detail_{};
  std::vector<Blocker> blockers_{};
};

}  // namespace drain
