#pragma once

// Drain Fabric -- the drain engine.
//
// The engine owns the authoritative state of the runtime: topology, obligations,
// the admission fence, evidence, authority, drain records, and drain sets. It
// performs exactly one lifecycle transition per drain per advance() call, and it
// never invokes a caller-supplied hook while holding its mutex: evacuation and
// restoration requests are collected under the lock and dispatched afterwards.
//
// Nothing in the engine performs maintenance, recomputes routing policy, or
// chooses upgrade versions. It closes admission, requests evacuation from
// adjacent runtimes, validates capacity and redundancy before each removal step,
// proves quiescence, and grants or withholds final drain authority.

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "drain/admission.hpp"
#include "drain/authority.hpp"
#include "drain/clock.hpp"
#include "drain/decision.hpp"
#include "drain/drain_set.hpp"
#include "drain/evidence.hpp"
#include "drain/explain.hpp"
#include "drain/export.hpp"
#include "drain/lifecycle.hpp"
#include "drain/obligation.hpp"
#include "drain/policy.hpp"
#include "drain/result.hpp"
#include "drain/topology.hpp"
#include "drain/version.hpp"

namespace drain {

inline constexpr std::size_t kMaxDrains = 65536;
inline constexpr std::size_t kMaxDrainSets = 16384;
inline constexpr std::size_t kMaxRenderBytes = 1u * 1024u * 1024u;

/// A request to drain one or more targets. The nonce makes the request
/// idempotent: replaying a request with the same nonce returns the existing
/// drain instead of creating a second one.
struct DrainRequest {
  std::vector<DrainTarget> targets{};
  std::string reason{};
  AuthorityId authority{};
  NodeId node{};
  RequestNonce nonce{};
};

/// An adjacent-runtime report about one obligation. The report is fenced by the
/// drain generation it belongs to, so a report issued before cancellation or
/// restoration cannot mutate current state.
struct ObligationReport {
  ObligationId obligation{};
  Revision expected_revision{};
  ObligationState next{ObligationState::Admitted};
  EvidenceSeq evidence{};
  DrainId drain{};
  Generation generation{};
  Attempt attempt{};
  std::string note{};
};

/// Request to an adjacent runtime to move or release an obligation. Drain Fabric
/// does not implement the movement.
struct EvacuationRequest {
  DrainId drain{};
  DrainTarget target{};
  ObligationId obligation{};
  /// Revision the adjacent runtime must present when it reports the obligation
  /// back. Carried in the request so a report can never be based on a stale
  /// read of the obligation.
  Revision expected_revision{};
  /// Evidence sequence the controller reserves for the adjacent runtime's
  /// release report. Zero means the runtime must supply its own.
  EvidenceSeq evidence_seq{};
  ObligationKind kind{ObligationKind::ActiveFlow};
  EvacuationMode mode{EvacuationMode::RerouteAndRelease};
  Generation generation{};
  Attempt attempt{};
  TimePoint at{0};
  std::string reason{};
};

/// Request to an adjacent runtime to abandon an evacuation and restore the
/// obligation.
struct RestorationRequest {
  DrainId drain{};
  DrainTarget target{};
  ObligationId obligation{};
  Generation generation{};
  Generation restore_generation{};
  TimePoint at{0};
  std::string reason{};
};

/// Request to an adjacent runtime to reopen its own admission for a target that
/// Drain Fabric is returning to service.
struct AdmissionReopenRequest {
  DrainTarget target{};
  Generation generation{};
  TimePoint at{0};
  std::string reason{};
};

/// Hook interface for adjacent runtimes. Implementations must not assume the
/// engine lock is held: it is never held during these calls, and an
/// implementation may safely call back into the engine.
class EvacuationSink {
 public:
  virtual ~EvacuationSink();
  virtual Status on_evacuation_request(const EvacuationRequest& request) = 0;
  virtual Status on_restoration_request(const RestorationRequest& request) = 0;
  virtual Status on_admission_reopen(const AdmissionReopenRequest& request) = 0;
};

class DrainEngine {
 public:
  explicit DrainEngine(Clock& clock);
  DrainEngine(DrainPolicy policy, Clock& clock);
  ~DrainEngine();

  DrainEngine(const DrainEngine&) = delete;
  DrainEngine& operator=(const DrainEngine&) = delete;

  // -- configuration ------------------------------------------------------
  DrainPolicy policy() const;
  Status set_policy(DrainPolicy policy, TimePoint now);
  void set_sink(EvacuationSink* sink);
  Clock& clock() const noexcept { return clock_; }
  TimePoint now() const { return clock_.now(); }

  // -- incarnation and authority -----------------------------------------
  Status install_incarnation(IncarnationId incarnation, TimePoint now);
  Status begin_new_epoch(IncarnationId incarnation, TimePoint now, std::string reason);

  /// Installs an explicit authority line (incarnation + epoch), revokes every
  /// existing token, and quarantines recovered evidence. The runtime uses this
  /// so that a hard kill cannot rewind the epoch.
  Status install_authority_line(IncarnationId incarnation, Epoch epoch, TimePoint now, std::string reason);
  IncarnationId incarnation() const;
  Epoch epoch() const;
  /// Alias used by the runtime when reconciling the boot marker with recovered
  /// authority state.
  Epoch authority_epoch() const { return epoch(); }
  Result<AuthorityToken> issue_authority(const DomainId& scope, Duration validity, TimePoint now);
  /// Validates a token presented by a peer against the live authority line.
  Status validate_authority(const AuthorityToken& token, TimePoint now) const;
  Status revoke_authority(AuthorityId id, std::string reason, TimePoint now);
  std::vector<AuthorityToken> authority_tokens() const;

  // -- topology -----------------------------------------------------------
  Status add_resource(ResourceNode node, TimePoint now);
  Status add_edge(const ResourceId& a, const ResourceId& b, TimePoint now);
  Status add_path(FabricPath path, TimePoint now);
  Status add_diversity_group(DiversityGroup group, TimePoint now);
  Status add_capacity_pool(CapacityPool pool, TimePoint now);
  Status add_protected_route(ProtectedRoute route, TimePoint now);
  Status report_resource_status(const ResourceId& id, ResourceStatus status, TimePoint now);
  Topology topology_snapshot() const;

  // -- obligations --------------------------------------------------------
  Result<Obligation> admit_obligation(Obligation draft, Generation presented, AuthorityId authority,
                                      TimePoint now);
  Status report_obligation(const ObligationReport& report, TimePoint now);
  std::vector<Obligation> obligations() const;
  std::optional<Obligation> obligation(ObligationId id) const;

  // -- evidence -----------------------------------------------------------
  Result<Evidence> record_evidence(Evidence draft, TimePoint now);

  /// Re-attests a quarantined evidence record. Only records produced by the
  /// live incarnation can be re-attested, so recovery never promotes stale
  /// evidence to current merely because it deserialized cleanly.
  Status attest_evidence(const EvidenceKey& key, EvidenceSeq seq, TimePoint now);

  std::vector<Evidence> evidence_records() const;

  // -- drains -------------------------------------------------------------
  Result<DrainId> request_drain(const DrainRequest& request, TimePoint now);
  Result<DrainSetId> request_drain_set(const DrainRequest& request, TimePoint now);
  Status cancel_drain(DrainId id, AuthorityId authority, std::string reason, TimePoint now);
  Status restore_drain(DrainId id, AuthorityId authority, std::string reason, TimePoint now);
  Status resume_drain(DrainId id, AuthorityId authority, TimePoint now);
  Status grant_exception(DrainId id, ObligationId obligation, Duration extension, AuthorityId authority,
                         TimePoint now);

  /// One scheduling pass. Each active drain advances by at most one transition.
  /// Requests collected during the pass are dispatched to the sink after the
  /// engine mutex has been released.
  Status advance(TimePoint now);

  // -- queries ------------------------------------------------------------
  std::vector<DrainRecord> drains() const;
  std::optional<DrainRecord> drain(DrainId id) const;
  std::vector<DrainSet> drain_sets() const;
  std::optional<DrainSet> drain_set(DrainSetId id) const;
  std::vector<Blocker> blockers(DrainId id) const;
  std::vector<Obligation> outstanding_obligations(const DrainTarget& target) const;
  std::vector<Decision> decisions(std::size_t limit) const;
  Explanation explain(DrainId id) const;
  AccountingReport audit(TimePoint now) const;
  std::uint64_t authoritative_digest() const;
  EngineStats stats() const;
  std::size_t outstanding_protected_count(const DrainTarget& target) const;
  std::vector<ExceptionGrant> exceptions(DrainId id) const;

  // -- persistence --------------------------------------------------------
  JsonValue snapshot() const;
  Status restore(const JsonValue& snapshot, IncarnationId previous_incarnation, TimePoint now);

  /// Diagnostics for the concurrency audit: true while the calling thread holds
  /// the engine mutex.
  bool lock_held_by_this_thread() const;

 private:
  struct PendingActions {
    std::vector<EvacuationRequest> evacuations{};
    std::vector<RestorationRequest> restorations{};
    std::vector<AdmissionReopenRequest> reopens{};
  };

  // The following helpers are called with mutex_ held.
  void step_drain(DrainRecord& record, TimePoint now, PendingActions& actions);
  std::vector<Blocker> evaluate_start(const DrainRecord& record, TimePoint now) const;
  std::vector<Blocker> evaluate_state(const DrainRecord& record, DrainState state, TimePoint now) const;
  std::vector<Blocker> evaluate_evacuation(const DrainRecord& record, TimePoint now) const;
  std::vector<Blocker> evaluate_quiescence(DrainRecord& record, TimePoint now,
                                           std::vector<ObligationId>& retirable) const;
  std::vector<Blocker> evaluate_verification(const DrainRecord& record, TimePoint now,
                                             EvidenceSeq& proof) const;
  bool enter_blocked(DrainRecord& record, std::vector<Blocker> blockers, TimePoint now, std::string action);
  bool apply_transition(DrainRecord& record, DrainState next, TimePoint now, std::string detail,
                        DecisionOutcome outcome, std::vector<std::string> inputs,
                        std::vector<std::string> evidence_notes, std::vector<std::string> rejected);
  Blocker make_blocker(BlockerCode code, std::string subject, std::string detail, TimePoint since,
                       TimePoint deadline) const;
  Blocker blocker_from_violation(const DrainRecord& record, const TopologyViolation& violation,
                                 TimePoint now) const;
  TimePoint grace_deadline_for(const DrainRecord& record, const Obligation& obligation) const;
  std::vector<const Obligation*> outstanding_for(const DrainTarget& target) const;
  void record_decision(Decision decision);
  Decision make_decision(std::string action, DecisionOutcome outcome, DrainId drain, DrainSetId set,
                         Generation generation, TimePoint now) const;
  /// Validates an authority token, preserving the exact fencing failure so that
  /// callers can distinguish "you were never allowed" from "you are stale".
  Status require_authority(AuthorityId authority, TimePoint now) const;
  /// True when this drain owns the removal of its target, derived from the
  /// authoritative fence rather than from a cached flag.
  bool removal_owned_by(const DrainRecord& record) const;
  /// Records a fresh removal observation for the drain generation. Used by the
  /// removal step and, after a restart, to re-observe a state the engine can
  /// still prove from live authoritative state.
  Status record_removal_evidence(const DrainRecord& record, TimePoint now);
  /// True when live authoritative state alone proves the removal step already
  /// happened for this drain generation.
  bool removal_is_reobservable(const DrainRecord& record) const;
  std::size_t active_drains_in_domain(const DomainId& domain, DrainId exclude) const;
  Status dispatch(PendingActions& actions, TimePoint now);
  std::uint64_t authoritative_digest_locked() const;
  EvidenceFence evidence_fence(TimePoint now) const;
  void clear_state_locked();

  mutable std::mutex mutex_{};
  mutable std::atomic<int> lock_depth_{0};

  Clock& clock_;
  DrainPolicy policy_{};
  EvacuationSink* sink_{nullptr};

  Topology topology_{};
  ObligationTable obligations_{};
  EvidenceLedger evidence_{};
  AuthorityRegistry authority_{};
  AdmissionFence fence_{};

  std::map<DrainId, DrainRecord> drains_{};
  std::map<DrainSetId, DrainSet> sets_{};
  std::map<DrainId, std::vector<ExceptionGrant>> exceptions_{};
  std::map<RequestNonce, DrainSetId> nonce_sets_{};
  std::deque<Decision> decisions_{};

  DrainId drain_high_water_{};
  DrainSetId set_high_water_{};
  std::uint32_t exception_high_water_{0};
  EngineStats stats_{};
};

}  // namespace drain
