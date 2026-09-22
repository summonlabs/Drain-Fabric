#include "drain/policy.hpp"

#include <algorithm>

#include "drain/digest.hpp"

namespace drain {
namespace {

constexpr std::size_t kKindCount = 6;

std::size_t kind_index(ObligationKind kind) { return static_cast<std::size_t>(kind); }

std::uint64_t read_u64(const JsonValue& value, std::string_view key, std::uint64_t fallback) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_uint(fallback);
}

std::int64_t read_i64(const JsonValue& value, std::string_view key, std::int64_t fallback) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_int(fallback);
}

bool read_bool(const JsonValue& value, std::string_view key, bool fallback) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_bool()) {
    return fallback;
  }
  return found->as_bool(fallback);
}

}  // namespace

DrainPolicy DrainPolicy::defaults() { return DrainPolicy{}; }

Duration DrainPolicy::grace_for_kind(ObligationKind kind) const {
  const Duration configured = grace_by_kind_[kind_index(kind)];
  return configured.count() == 0 ? default_grace_ : configured;
}

void DrainPolicy::set_grace_for_kind(ObligationKind kind, Duration value) {
  grace_by_kind_[kind_index(kind)] = value;
}

Duration DrainPolicy::grace_for(const Obligation& obligation) const {
  return obligation.grace().count() == 0 ? grace_for_kind(obligation.kind()) : obligation.grace();
}

Status DrainPolicy::validate() const {
  if (name_.empty() || name_.size() > 128) {
    return fail(ErrorCode::PolicyViolation, "policy name must be non-empty and at most 128 bytes");
  }
  if (max_targets_per_request_ == 0 || max_targets_per_request_ > kHardMaxTargetsPerRequest) {
    return fail(ErrorCode::PolicyViolation, "max_targets_per_request is outside the supported range");
  }
  if (max_drains_in_set_ == 0 || max_drains_in_set_ > kHardMaxDrainsInSet) {
    return fail(ErrorCode::PolicyViolation, "max_drains_in_set is outside the supported range");
  }
  if (max_concurrent_drains_per_domain_ == 0 ||
      max_concurrent_drains_per_domain_ > kHardMaxConcurrentDrainsPerDomain) {
    return fail(ErrorCode::PolicyViolation,
                "max_concurrent_drains_per_domain is outside the supported range");
  }
  if (max_obligations_ == 0 || max_obligations_ > kHardMaxObligations) {
    return fail(ErrorCode::PolicyViolation, "max_obligations is outside the supported range");
  }
  if (max_evidence_records_ == 0 || max_evidence_records_ > kHardMaxEvidenceRecords) {
    return fail(ErrorCode::PolicyViolation, "max_evidence_records is outside the supported range");
  }
  if (max_decisions_retained_ == 0 || max_decisions_retained_ > kHardMaxDecisionsRetained) {
    return fail(ErrorCode::PolicyViolation, "max_decisions_retained is outside the supported range");
  }
  if (max_blockers_per_drain_ == 0 || max_blockers_per_drain_ > kHardMaxBlockersPerDrain) {
    return fail(ErrorCode::PolicyViolation, "max_blockers_per_drain is outside the supported range");
  }
  if (max_evacuation_attempts_ == 0 || max_evacuation_attempts_ > kHardMaxEvacuationAttempts) {
    return fail(ErrorCode::PolicyViolation, "max_evacuation_attempts is outside the supported range");
  }
  if (max_exception_extensions_ > kHardMaxExceptionExtensions) {
    return fail(ErrorCode::PolicyViolation, "max_exception_extensions is outside the supported range");
  }
  if (admission_fence_settle_.count() < 0) {
    return fail(ErrorCode::PolicyViolation, "admission_fence_settle must not be negative");
  }
  if (evidence_freshness_.count() <= 0) {
    return fail(ErrorCode::PolicyViolation, "evidence_freshness must be positive");
  }
  if (evacuation_retry_interval_.count() < 0) {
    return fail(ErrorCode::PolicyViolation, "evacuation_retry_interval must not be negative");
  }
  if (evidence_clock_skew_.count() < 0) {
    return fail(ErrorCode::PolicyViolation, "evidence_clock_skew must not be negative");
  }
  if (default_grace_.count() <= 0) {
    return fail(ErrorCode::PolicyViolation, "default_grace must be positive");
  }
  for (std::size_t index = 0; index < kKindCount; ++index) {
    if (grace_by_kind_[index].count() < 0) {
      return fail(ErrorCode::PolicyViolation, "per-kind grace must not be negative");
    }
  }
  if (capacity_headroom_denominator_ == 0) {
    return fail(ErrorCode::PolicyViolation, "capacity headroom denominator must be non-zero");
  }
  if (capacity_headroom_numerator_ == 0) {
    return fail(ErrorCode::PolicyViolation, "capacity headroom numerator must be non-zero");
  }
  if (required_path_diversity_.is_zero()) {
    return fail(ErrorCode::PolicyViolation, "required_path_diversity must be at least one");
  }
  if (max_snapshot_bytes_.is_zero() || max_snapshot_bytes_.value() > kHardMaxSnapshotBytes) {
    return fail(ErrorCode::PolicyViolation, "max_snapshot_bytes is outside the supported range");
  }
  if (max_frame_bytes_.is_zero() || max_frame_bytes_.value() > kHardMaxFrameBytes) {
    return fail(ErrorCode::PolicyViolation, "max_frame_bytes is outside the supported range");
  }
  if (max_connections_ == 0 || max_connections_ > kHardMaxConnections) {
    return fail(ErrorCode::PolicyViolation, "max_connections is outside the supported range");
  }
  if (max_worker_threads_ == 0 || max_worker_threads_ > kHardMaxWorkerThreads) {
    return fail(ErrorCode::PolicyViolation, "max_worker_threads is outside the supported range");
  }
  if (max_pending_work_ == 0 || max_pending_work_ > 1000000) {
    return fail(ErrorCode::PolicyViolation, "max_pending_work is outside the supported range");
  }
  return ok_status();
}

std::string DrainPolicy::fingerprint() const {
  Digest64 digest;
  digest.update_tagged("name", name_);
  digest.update_u64(revision_.value());
  digest.update_u64(max_targets_per_request_);
  digest.update_u64(max_drains_in_set_);
  digest.update_u64(max_concurrent_drains_per_domain_);
  digest.update_bool(serialize_same_domain_in_set_);
  digest.update_u64(max_obligations_);
  digest.update_u64(max_evidence_records_);
  digest.update_u64(max_decisions_retained_);
  digest.update_u64(max_blockers_per_drain_);
  digest.update_u64(max_evacuation_attempts_);
  digest.update_u64(max_exception_extensions_);
  digest.update_u64(static_cast<std::uint64_t>(admission_fence_settle_.count()));
  digest.update_u64(static_cast<std::uint64_t>(evidence_freshness_.count()));
  digest.update_u64(static_cast<std::uint64_t>(evacuation_retry_interval_.count()));
  digest.update_u64(static_cast<std::uint64_t>(evidence_clock_skew_.count()));
  digest.update_u64(static_cast<std::uint64_t>(default_grace_.count()));
  for (std::size_t index = 0; index < kKindCount; ++index) {
    digest.update_u64(static_cast<std::uint64_t>(grace_by_kind_[index].count()));
  }
  digest.update_u64(capacity_headroom_numerator_);
  digest.update_u64(capacity_headroom_denominator_);
  digest.update_u64(required_path_diversity_.value());
  digest.update_bool(require_release_evidence_);
  digest.update_bool(require_quiescence_evidence_);
  digest.update_bool(evidence_requires_live_incarnation_);
  digest.update_bool(reobserve_removal_evidence_);
  digest.update_u64(max_snapshot_bytes_.value());
  digest.update_u64(max_frame_bytes_.value());
  digest.update_u64(max_connections_);
  digest.update_u64(max_worker_threads_);
  digest.update_u64(max_pending_work_);
  return hex_u64(digest.value());
}

JsonValue DrainPolicy::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("name", JsonValue(name_));
  value.set("revision", JsonValue(revision_.value()));
  value.set("max_targets_per_request", JsonValue(static_cast<std::uint64_t>(max_targets_per_request_)));
  value.set("max_drains_in_set", JsonValue(static_cast<std::uint64_t>(max_drains_in_set_)));
  value.set("max_concurrent_drains_per_domain",
            JsonValue(static_cast<std::uint64_t>(max_concurrent_drains_per_domain_)));
  value.set("serialize_same_domain_in_set", JsonValue(serialize_same_domain_in_set_));
  value.set("max_obligations", JsonValue(static_cast<std::uint64_t>(max_obligations_)));
  value.set("max_evidence_records", JsonValue(static_cast<std::uint64_t>(max_evidence_records_)));
  value.set("max_decisions_retained", JsonValue(static_cast<std::uint64_t>(max_decisions_retained_)));
  value.set("max_blockers_per_drain", JsonValue(static_cast<std::uint64_t>(max_blockers_per_drain_)));
  value.set("max_evacuation_attempts", JsonValue(static_cast<std::uint64_t>(max_evacuation_attempts_)));
  value.set("max_exception_extensions", JsonValue(static_cast<std::uint64_t>(max_exception_extensions_)));
  value.set("admission_fence_settle_ns",
            JsonValue(static_cast<std::uint64_t>(admission_fence_settle_.count())));
  value.set("evidence_freshness_ns", JsonValue(static_cast<std::uint64_t>(evidence_freshness_.count())));
  value.set("evacuation_retry_interval_ns",
            JsonValue(static_cast<std::uint64_t>(evacuation_retry_interval_.count())));
  value.set("evidence_clock_skew_ns", JsonValue(static_cast<std::uint64_t>(evidence_clock_skew_.count())));
  value.set("default_grace_ns", JsonValue(static_cast<std::uint64_t>(default_grace_.count())));
  JsonValue graces = JsonValue::object();
  for (std::size_t index = 0; index < kKindCount; ++index) {
    graces.set(drain::to_string(static_cast<ObligationKind>(index)),
               JsonValue(static_cast<std::uint64_t>(grace_by_kind_[index].count())));
  }
  value.set("grace_by_kind_ns", std::move(graces));
  value.set("capacity_headroom_numerator", JsonValue(capacity_headroom_numerator_));
  value.set("capacity_headroom_denominator", JsonValue(capacity_headroom_denominator_));
  value.set("required_path_diversity", JsonValue(required_path_diversity_.value()));
  value.set("require_release_evidence", JsonValue(require_release_evidence_));
  value.set("require_quiescence_evidence", JsonValue(require_quiescence_evidence_));
  value.set("evidence_requires_live_incarnation", JsonValue(evidence_requires_live_incarnation_));
  value.set("reobserve_removal_evidence", JsonValue(reobserve_removal_evidence_));
  value.set("max_snapshot_bytes", JsonValue(max_snapshot_bytes_.value()));
  value.set("max_frame_bytes", JsonValue(max_frame_bytes_.value()));
  value.set("max_connections", JsonValue(static_cast<std::uint64_t>(max_connections_)));
  value.set("max_worker_threads", JsonValue(static_cast<std::uint64_t>(max_worker_threads_)));
  value.set("max_pending_work", JsonValue(static_cast<std::uint64_t>(max_pending_work_)));
  value.set("fingerprint", JsonValue(fingerprint()));
  return value;
}

Result<DrainPolicy> DrainPolicy::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PolicyViolation, "policy document must be a json object");
  }
  DrainPolicy policy;
  policy.set_name(value.get_string("name", "default"));
  policy.set_revision(Revision{read_u64(value, "revision", 1)});
  if (policy.revision().is_zero()) {
    return Error(ErrorCode::PolicyViolation, "policy revision must be non-zero");
  }
  const auto sized = [&](std::string_view key, std::size_t fallback) -> Result<std::size_t> {
    const std::uint64_t raw = read_u64(value, key, static_cast<std::uint64_t>(fallback));
    const auto narrowed = narrow_checked<std::size_t>(raw);
    if (!narrowed.has_value()) {
      return Error(ErrorCode::PolicyViolation, std::string("policy field ") + std::string(key) +
                                                   " does not fit the platform size type");
    }
    return *narrowed;
  };

  auto targets = sized("max_targets_per_request", policy.max_targets_per_request());
  if (!targets.ok()) return targets.error();
  policy.set_max_targets_per_request(*targets);
  auto set_size = sized("max_drains_in_set", policy.max_drains_in_set());
  if (!set_size.ok()) return set_size.error();
  policy.set_max_drains_in_set(*set_size);
  auto per_domain = sized("max_concurrent_drains_per_domain", policy.max_concurrent_drains_per_domain());
  if (!per_domain.ok()) return per_domain.error();
  policy.set_max_concurrent_drains_per_domain(*per_domain);
  policy.set_serialize_same_domain_in_set(
      read_bool(value, "serialize_same_domain_in_set", policy.serialize_same_domain_in_set()));
  auto obligations = sized("max_obligations", policy.max_obligations());
  if (!obligations.ok()) return obligations.error();
  policy.set_max_obligations(*obligations);
  auto evidence = sized("max_evidence_records", policy.max_evidence_records());
  if (!evidence.ok()) return evidence.error();
  policy.set_max_evidence_records(*evidence);
  auto decisions = sized("max_decisions_retained", policy.max_decisions_retained());
  if (!decisions.ok()) return decisions.error();
  policy.set_max_decisions_retained(*decisions);
  auto blockers = sized("max_blockers_per_drain", policy.max_blockers_per_drain());
  if (!blockers.ok()) return blockers.error();
  policy.set_max_blockers_per_drain(*blockers);

  const std::uint64_t attempts = read_u64(value, "max_evacuation_attempts", policy.max_evacuation_attempts());
  const auto narrowed_attempts = narrow_checked<std::uint32_t>(attempts);
  if (!narrowed_attempts.has_value()) {
    return Error(ErrorCode::PolicyViolation, "max_evacuation_attempts does not fit a 32-bit counter");
  }
  policy.set_max_evacuation_attempts(*narrowed_attempts);

  const std::uint64_t extensions = read_u64(value, "max_exception_extensions", policy.max_exception_extensions());
  const auto narrowed_extensions = narrow_checked<std::uint32_t>(extensions);
  if (!narrowed_extensions.has_value()) {
    return Error(ErrorCode::PolicyViolation, "max_exception_extensions does not fit a 32-bit counter");
  }
  policy.set_max_exception_extensions(*narrowed_extensions);

  policy.set_admission_fence_settle(
      Duration{read_i64(value, "admission_fence_settle_ns", policy.admission_fence_settle().count())});
  policy.set_evidence_freshness(
      Duration{read_i64(value, "evidence_freshness_ns", policy.evidence_freshness().count())});
  policy.set_evacuation_retry_interval(
      Duration{read_i64(value, "evacuation_retry_interval_ns", policy.evacuation_retry_interval().count())});
  policy.set_default_grace(Duration{read_i64(value, "default_grace_ns", policy.default_grace().count())});
  policy.set_evidence_clock_skew(
      Duration{read_i64(value, "evidence_clock_skew_ns", policy.evidence_clock_skew().count())});

  if (const JsonValue* graces = value.find("grace_by_kind_ns"); graces != nullptr && graces->is_object()) {
    for (std::size_t index = 0; index < kKindCount; ++index) {
      const auto kind = static_cast<ObligationKind>(index);
      const JsonValue* entry = graces->find(drain::to_string(kind));
      if (entry == nullptr || !entry->is_number()) {
        continue;
      }
      const std::int64_t raw = entry->as_int(-1);
      if (raw < 0) {
        return Error(ErrorCode::PolicyViolation, "per-kind grace must not be negative");
      }
      policy.set_grace_for_kind(kind, Duration{raw});
    }
  }

  policy.set_capacity_headroom(read_u64(value, "capacity_headroom_numerator", policy.capacity_headroom_numerator()),
                               read_u64(value, "capacity_headroom_denominator",
                                        policy.capacity_headroom_denominator()));
  policy.set_required_path_diversity(
      MemberCount::from(read_u64(value, "required_path_diversity", policy.required_path_diversity().value())));
  policy.set_require_release_evidence(
      read_bool(value, "require_release_evidence", policy.require_release_evidence()));
  policy.set_require_quiescence_evidence(
      read_bool(value, "require_quiescence_evidence", policy.require_quiescence_evidence()));
  policy.set_evidence_requires_live_incarnation(
      read_bool(value, "evidence_requires_live_incarnation", policy.evidence_requires_live_incarnation()));
  policy.set_reobserve_removal_evidence(
      read_bool(value, "reobserve_removal_evidence", policy.reobserve_removal_evidence()));
  policy.set_max_snapshot_bytes(
      ByteCount::from(read_u64(value, "max_snapshot_bytes", policy.max_snapshot_bytes().value())));
  policy.set_max_frame_bytes(ByteCount::from(read_u64(value, "max_frame_bytes", policy.max_frame_bytes().value())));
  auto connections = sized("max_connections", policy.max_connections());
  if (!connections.ok()) return connections.error();
  policy.set_max_connections(*connections);
  auto workers = sized("max_worker_threads", policy.max_worker_threads());
  if (!workers.ok()) return workers.error();
  policy.set_max_worker_threads(*workers);
  auto pending = sized("max_pending_work", policy.max_pending_work());
  if (!pending.ok()) return pending.error();
  policy.set_max_pending_work(*pending);

  Status valid = policy.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  return policy;
}

}  // namespace drain
