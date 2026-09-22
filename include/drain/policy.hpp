#pragma once

// Drain Fabric -- deterministic drain policy.
//
// Policy is data, not code: every decision the engine makes cites the policy
// revision and fingerprint that governed it. Policy carries no wall-clock
// timeouts; it carries deadlines, which produce blockers and exception
// requirements rather than silent failures.
//
// All ratios are integer numerator/denominator pairs so that capacity and
// redundancy comparisons are bit-identical on every platform.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "drain/clock.hpp"
#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/obligation.hpp"
#include "drain/result.hpp"

namespace drain {

/// Hard caps. A policy document may lower these, never raise them.
inline constexpr std::size_t kHardMaxTargetsPerRequest = 256;
inline constexpr std::size_t kHardMaxDrainsInSet = 256;
inline constexpr std::size_t kHardMaxConcurrentDrainsPerDomain = 64;
inline constexpr std::size_t kHardMaxObligations = 1000000;
inline constexpr std::size_t kHardMaxEvidenceRecords = 1000000;
inline constexpr std::size_t kHardMaxDecisionsRetained = 100000;
inline constexpr std::size_t kHardMaxBlockersPerDrain = 256;
inline constexpr std::uint32_t kHardMaxEvacuationAttempts = 1000;
inline constexpr std::uint32_t kHardMaxExceptionExtensions = 64;
inline constexpr std::uint64_t kHardMaxSnapshotBytes = 256ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kHardMaxFrameBytes = 16ull * 1024ull * 1024ull;
inline constexpr std::size_t kHardMaxConnections = 4096;
inline constexpr std::size_t kHardMaxWorkerThreads = 256;

class DrainPolicy {
 public:
  static DrainPolicy defaults();

  const std::string& name() const noexcept { return name_; }
  void set_name(std::string name) noexcept { name_ = std::move(name); }

  Revision revision() const noexcept { return revision_; }
  void set_revision(Revision revision) noexcept { revision_ = revision; }

  std::size_t max_targets_per_request() const noexcept { return max_targets_per_request_; }
  void set_max_targets_per_request(std::size_t value) noexcept { max_targets_per_request_ = value; }

  std::size_t max_drains_in_set() const noexcept { return max_drains_in_set_; }
  void set_max_drains_in_set(std::size_t value) noexcept { max_drains_in_set_ = value; }

  std::size_t max_concurrent_drains_per_domain() const noexcept { return max_concurrent_drains_per_domain_; }
  void set_max_concurrent_drains_per_domain(std::size_t value) noexcept {
    max_concurrent_drains_per_domain_ = value;
  }

  /// When true, members of one drain set that share a failure domain are
  /// serialised by their deterministic set order in addition to the domain
  /// concurrency limit.
  bool serialize_same_domain_in_set() const noexcept { return serialize_same_domain_in_set_; }
  void set_serialize_same_domain_in_set(bool value) noexcept { serialize_same_domain_in_set_ = value; }

  std::size_t max_obligations() const noexcept { return max_obligations_; }
  void set_max_obligations(std::size_t value) noexcept { max_obligations_ = value; }

  std::size_t max_evidence_records() const noexcept { return max_evidence_records_; }
  void set_max_evidence_records(std::size_t value) noexcept { max_evidence_records_ = value; }

  std::size_t max_decisions_retained() const noexcept { return max_decisions_retained_; }
  void set_max_decisions_retained(std::size_t value) noexcept { max_decisions_retained_ = value; }

  std::size_t max_blockers_per_drain() const noexcept { return max_blockers_per_drain_; }
  void set_max_blockers_per_drain(std::size_t value) noexcept { max_blockers_per_drain_ = value; }

  std::uint32_t max_evacuation_attempts() const noexcept { return max_evacuation_attempts_; }
  void set_max_evacuation_attempts(std::uint32_t value) noexcept { max_evacuation_attempts_ = value; }

  std::uint32_t max_exception_extensions() const noexcept { return max_exception_extensions_; }
  void set_max_exception_extensions(std::uint32_t value) noexcept { max_exception_extensions_ = value; }

  /// Policy-defined boundary settling interval: the time that must pass after
  /// admission is closed before any evacuation request is issued. This is not a
  /// timeout; it is the interval during which the closure must have propagated.
  Duration admission_fence_settle() const noexcept { return admission_fence_settle_; }
  void set_admission_fence_settle(Duration value) noexcept { admission_fence_settle_ = value; }

  /// How long a piece of evidence stays usable. Evidence older than this is
  /// reported as stale and can never complete a drain.
  Duration evidence_freshness() const noexcept { return evidence_freshness_; }
  void set_evidence_freshness(Duration value) noexcept { evidence_freshness_ = value; }

  /// Minimum spacing between repeat evacuation requests for the same obligation.
  Duration evacuation_retry_interval() const noexcept { return evacuation_retry_interval_; }
  void set_evacuation_retry_interval(Duration value) noexcept { evacuation_retry_interval_ = value; }

  /// Bounded clock skew tolerated for observations made by an adjacent runtime.
  /// A peer's observation may be slightly ahead of the local clock; anything
  /// beyond this window is rejected as forged or badly skewed.
  Duration evidence_clock_skew() const noexcept { return evidence_clock_skew_; }
  void set_evidence_clock_skew(Duration value) noexcept { evidence_clock_skew_ = value; }

  Duration default_grace() const noexcept { return default_grace_; }
  void set_default_grace(Duration value) noexcept { default_grace_ = value; }

  /// Per-kind grace override. A zero value falls back to default_grace.
  Duration grace_for_kind(ObligationKind kind) const;
  void set_grace_for_kind(ObligationKind kind, Duration value);

  /// Effective grace for an obligation: its own override, else the kind policy.
  Duration grace_for(const Obligation& obligation) const;

  std::uint64_t capacity_headroom_numerator() const noexcept { return capacity_headroom_numerator_; }
  std::uint64_t capacity_headroom_denominator() const noexcept { return capacity_headroom_denominator_; }
  void set_capacity_headroom(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    capacity_headroom_numerator_ = numerator;
    capacity_headroom_denominator_ = denominator;
  }

  MemberCount required_path_diversity() const noexcept { return required_path_diversity_; }
  void set_required_path_diversity(MemberCount value) noexcept { required_path_diversity_ = value; }

  bool require_release_evidence() const noexcept { return require_release_evidence_; }
  void set_require_release_evidence(bool value) noexcept { require_release_evidence_ = value; }

  bool require_quiescence_evidence() const noexcept { return require_quiescence_evidence_; }
  void set_require_quiescence_evidence(bool value) noexcept { require_quiescence_evidence_ = value; }

  /// When true, evidence produced by a previous incarnation is never usable,
  /// even if it has not expired. This is the conservative restart default.
  bool evidence_requires_live_incarnation() const noexcept { return evidence_requires_live_incarnation_; }
  void set_evidence_requires_live_incarnation(bool value) noexcept {
    evidence_requires_live_incarnation_ = value;
  }

  /// When true, a drain that would otherwise be blocked only by a missing or
  /// quarantined removal observation may re-observe it from live authoritative
  /// state (fence closed at the matching generation, resource out of service,
  /// zero outstanding protected dependencies). This is a *new* observation, not
  /// reuse of recovered evidence, and it is what makes restart recovery able to
  /// finish a drain that is genuinely complete.
  bool reobserve_removal_evidence() const noexcept { return reobserve_removal_evidence_; }
  void set_reobserve_removal_evidence(bool value) noexcept { reobserve_removal_evidence_ = value; }

  ByteCount max_snapshot_bytes() const noexcept { return max_snapshot_bytes_; }
  void set_max_snapshot_bytes(ByteCount value) noexcept { max_snapshot_bytes_ = value; }

  ByteCount max_frame_bytes() const noexcept { return max_frame_bytes_; }
  void set_max_frame_bytes(ByteCount value) noexcept { max_frame_bytes_ = value; }

  std::size_t max_connections() const noexcept { return max_connections_; }
  void set_max_connections(std::size_t value) noexcept { max_connections_ = value; }

  std::size_t max_worker_threads() const noexcept { return max_worker_threads_; }
  void set_max_worker_threads(std::size_t value) noexcept { max_worker_threads_ = value; }

  std::size_t max_pending_work() const noexcept { return max_pending_work_; }
  void set_max_pending_work(std::size_t value) noexcept { max_pending_work_ = value; }

  /// Validates every field against the hard caps. A policy that fails this check
  /// is never installed.
  Status validate() const;

  /// Stable content fingerprint. Two policies with the same fingerprint must
  /// produce identical decisions.
  std::string fingerprint() const;

  JsonValue to_json() const;
  static Result<DrainPolicy> from_json(const JsonValue& value);

 private:
  std::string name_{"default"};
  Revision revision_{Revision{1}};
  std::size_t max_targets_per_request_{64};
  std::size_t max_drains_in_set_{32};
  std::size_t max_concurrent_drains_per_domain_{1};
  bool serialize_same_domain_in_set_{true};
  std::size_t max_obligations_{100000};
  std::size_t max_evidence_records_{200000};
  std::size_t max_decisions_retained_{4096};
  std::size_t max_blockers_per_drain_{64};
  std::uint32_t max_evacuation_attempts_{8};
  std::uint32_t max_exception_extensions_{4};
  Duration admission_fence_settle_{0};
  Duration evidence_freshness_{std::chrono::seconds(300)};
  Duration evacuation_retry_interval_{std::chrono::seconds(5)};
  Duration evidence_clock_skew_{std::chrono::seconds(5)};
  Duration default_grace_{std::chrono::seconds(3600)};
  std::array<Duration, 6> grace_by_kind_{};
  std::uint64_t capacity_headroom_numerator_{1};
  std::uint64_t capacity_headroom_denominator_{1};
  MemberCount required_path_diversity_{MemberCount::from(1)};
  bool require_release_evidence_{true};
  bool require_quiescence_evidence_{true};
  bool evidence_requires_live_incarnation_{true};
  bool reobserve_removal_evidence_{true};
  ByteCount max_snapshot_bytes_{ByteCount::from(32ull * 1024ull * 1024ull)};
  ByteCount max_frame_bytes_{ByteCount::from(1024ull * 1024ull)};
  std::size_t max_connections_{64};
  std::size_t max_worker_threads_{8};
  std::size_t max_pending_work_{1024};
};

}  // namespace drain
