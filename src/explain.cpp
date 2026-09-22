#include "drain/explain.hpp"

#include <algorithm>

#include "drain/checked.hpp"
#include "drain/clock.hpp"
#include "drain/digest.hpp"

namespace drain {

JsonValue ExceptionGrant::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("drain", JsonValue(drain.value()));
  value.set("obligation", JsonValue(obligation.value()));
  value.set("extension_ns", JsonValue(static_cast<std::uint64_t>(extension.count())));
  value.set("authority", JsonValue(authority.value()));
  value.set("granted_at_ns", JsonValue(static_cast<std::uint64_t>(granted_at.count())));
  value.set("sequence", JsonValue(static_cast<std::uint64_t>(sequence)));
  return value;
}

Result<ExceptionGrant> ExceptionGrant::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "exception grant is not an object");
  }
  ExceptionGrant grant;
  grant.drain = DrainId{value.get_uint("drain")};
  grant.obligation = ObligationId{value.get_uint("obligation")};
  grant.extension = Duration{value.get_int("extension_ns")};
  grant.authority = AuthorityId{value.get_uint("authority")};
  grant.granted_at = TimePoint{value.get_int("granted_at_ns")};
  const std::uint64_t sequence = value.get_uint("sequence");
  const auto narrowed = narrow_checked<std::uint32_t>(sequence);
  if (!narrowed.has_value()) {
    return Error(ErrorCode::BoundsExceeded, "exception grant sequence does not fit a 32-bit counter");
  }
  grant.sequence = *narrowed;
  if (grant.extension.count() <= 0) {
    return Error(ErrorCode::PersistenceCorrupt, "exception grant extension must be positive");
  }
  return grant;
}

JsonValue EngineStats::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("advances", JsonValue(advances));
  value.set("transitions", JsonValue(transitions));
  value.set("evacuation_requests", JsonValue(evacuation_requests));
  value.set("restoration_requests", JsonValue(restoration_requests));
  value.set("reopen_requests", JsonValue(reopen_requests));
  value.set("rejected_reports", JsonValue(rejected_reports));
  value.set("rejected_admissions", JsonValue(rejected_admissions));
  value.set("rejected_evidence", JsonValue(rejected_evidence));
  value.set("blocked_evaluations", JsonValue(blocked_evaluations));
  value.set("sink_failures", JsonValue(sink_failures));
  value.set("completion_proofs", JsonValue(completion_proofs));
  return value;
}

std::string EngineStats::render() const {
  std::string out;
  out.append("advances=");
  out.append(std::to_string(advances));
  out.append(" transitions=");
  out.append(std::to_string(transitions));
  out.append(" evacuation-requests=");
  out.append(std::to_string(evacuation_requests));
  out.append(" restoration-requests=");
  out.append(std::to_string(restoration_requests));
  out.append(" reopen-requests=");
  out.append(std::to_string(reopen_requests));
  out.append(" rejected-reports=");
  out.append(std::to_string(rejected_reports));
  out.append(" rejected-admissions=");
  out.append(std::to_string(rejected_admissions));
  out.append(" rejected-evidence=");
  out.append(std::to_string(rejected_evidence));
  out.append(" blocked-evaluations=");
  out.append(std::to_string(blocked_evaluations));
  out.append(" sink-failures=");
  out.append(std::to_string(sink_failures));
  out.append(" completion-proofs=");
  out.append(std::to_string(completion_proofs));
  return out;
}

JsonValue Explanation::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("found", JsonValue(found));
  value.set("summary", JsonValue(summary));
  if (found) {
    value.set("drain", drain.to_json());
  }
  JsonValue blockers_json = JsonValue::array();
  for (const auto& blocker : blockers) {
    blockers_json.array_ref().push_back(blocker.to_json());
  }
  value.set("blockers", std::move(blockers_json));

  JsonValue dependents_json = JsonValue::array();
  for (const auto& obligation : dependents) {
    dependents_json.array_ref().push_back(obligation.to_json());
  }
  value.set("dependents", std::move(dependents_json));

  JsonValue outstanding_json = JsonValue::array();
  for (const auto& obligation : outstanding) {
    outstanding_json.array_ref().push_back(obligation.to_json());
  }
  value.set("outstanding", std::move(outstanding_json));

  JsonValue history_json = JsonValue::array();
  for (const auto& decision : history) {
    history_json.array_ref().push_back(decision.to_json());
  }
  value.set("history", std::move(history_json));
  return value;
}

std::string Explanation::render() const {
  std::string out;
  if (!found) {
    out.append("drain not found\n");
    return out;
  }
  out.append("drain ");
  out.append(drain::to_string(drain.id()));
  out.append(" target=");
  out.append(drain.target().to_string());
  out.append(" state=");
  out.append(drain::to_string(drain.state()));
  out.append(" generation=");
  out.append(drain::to_string(drain.generation()));
  out.append(" revision=");
  out.append(drain::to_string(drain.revision()));
  out.append("\n");
  out.append("summary: ");
  out.append(summary);
  out.append("\n");
  out.append("blockers: ");
  out.append(std::to_string(blockers.size()));
  out.append("\n");
  for (const auto& blocker : blockers) {
    out.append("  ");
    out.append(blocker.render());
    out.append("\n");
  }
  out.append("dependents: ");
  out.append(std::to_string(dependents.size()));
  out.append("\n");
  for (const auto& obligation : dependents) {
    out.append("  ");
    out.append(obligation.label());
    out.append(" state=");
    out.append(drain::to_string(obligation.state()));
    out.append(" protection=");
    out.append(drain::to_string(obligation.protection()));
    out.append("\n");
  }
  out.append("outstanding-protected: ");
  out.append(std::to_string(outstanding.size()));
  out.append("\n");
  for (const auto& obligation : outstanding) {
    out.append("  ");
    out.append(obligation.label());
    out.append(" state=");
    out.append(drain::to_string(obligation.state()));
    out.append("\n");
  }
  out.append("history: ");
  out.append(std::to_string(history.size()));
  out.append(" decision(s)\n");
  for (const auto& decision : history) {
    out.append("  ");
    out.append(decision.render());
    out.append("\n");
  }
  return out;
}

JsonValue AccountingReport::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("drains_total", JsonValue(static_cast<std::uint64_t>(drains_total)));
  value.set("drains_active", JsonValue(static_cast<std::uint64_t>(drains_active)));
  value.set("drains_blocked", JsonValue(static_cast<std::uint64_t>(drains_blocked)));
  value.set("drains_drained", JsonValue(static_cast<std::uint64_t>(drains_drained)));
  value.set("drains_cancelled", JsonValue(static_cast<std::uint64_t>(drains_cancelled)));
  value.set("drains_failed", JsonValue(static_cast<std::uint64_t>(drains_failed)));
  value.set("drains_restoring", JsonValue(static_cast<std::uint64_t>(drains_restoring)));
  value.set("drains_restored", JsonValue(static_cast<std::uint64_t>(drains_restored)));
  value.set("obligations_total", JsonValue(static_cast<std::uint64_t>(obligations_total)));
  value.set("obligations_outstanding_protected",
            JsonValue(static_cast<std::uint64_t>(obligations_outstanding_protected)));
  value.set("obligations_retired", JsonValue(static_cast<std::uint64_t>(obligations_retired)));
  value.set("obligations_reappeared", JsonValue(static_cast<std::uint64_t>(obligations_reappeared)));
  value.set("fences_closed", JsonValue(static_cast<std::uint64_t>(fences_closed)));
  value.set("evidence_records", JsonValue(static_cast<std::uint64_t>(evidence_records)));
  value.set("drain_sets", JsonValue(static_cast<std::uint64_t>(drain_sets)));
  value.set("authoritative_digest", JsonValue(authoritative_digest));
  value.set("topology_digest", JsonValue(topology_digest));
  value.set("obligation_digest", JsonValue(obligation_digest));
  value.set("evidence_digest", JsonValue(evidence_digest));
  JsonValue violations_json = JsonValue::array();
  for (const auto& violation : violations) {
    violations_json.array_ref().push_back(JsonValue(violation));
  }
  value.set("violations", std::move(violations_json));
  value.set("clean", JsonValue(clean()));
  return value;
}

std::string AccountingReport::render() const {
  std::string out;
  out.append("drains total=");
  out.append(std::to_string(drains_total));
  out.append(" active=");
  out.append(std::to_string(drains_active));
  out.append(" blocked=");
  out.append(std::to_string(drains_blocked));
  out.append(" drained=");
  out.append(std::to_string(drains_drained));
  out.append(" cancelled=");
  out.append(std::to_string(drains_cancelled));
  out.append(" failed=");
  out.append(std::to_string(drains_failed));
  out.append(" restoring=");
  out.append(std::to_string(drains_restoring));
  out.append(" restored=");
  out.append(std::to_string(drains_restored));
  out.append("\n");
  out.append("obligations total=");
  out.append(std::to_string(obligations_total));
  out.append(" outstanding-protected=");
  out.append(std::to_string(obligations_outstanding_protected));
  out.append(" retired=");
  out.append(std::to_string(obligations_retired));
  out.append(" reappeared=");
  out.append(std::to_string(obligations_reappeared));
  out.append("\n");
  out.append("fences-closed=");
  out.append(std::to_string(fences_closed));
  out.append(" evidence-records=");
  out.append(std::to_string(evidence_records));
  out.append(" drain-sets=");
  out.append(std::to_string(drain_sets));
  out.append("\n");
  out.append("authoritative-digest=");
  out.append(hex_u64(authoritative_digest));
  out.append(" topology-digest=");
  out.append(hex_u64(topology_digest));
  out.append(" obligation-digest=");
  out.append(hex_u64(obligation_digest));
  out.append(" evidence-digest=");
  out.append(hex_u64(evidence_digest));
  out.append("\n");
  out.append("violations: ");
  out.append(std::to_string(violations.size()));
  out.append("\n");
  for (const auto& violation : violations) {
    out.append("  ");
    out.append(violation);
    out.append("\n");
  }
  return out;
}

}  // namespace drain
