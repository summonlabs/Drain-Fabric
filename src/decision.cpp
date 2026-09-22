#include "drain/decision.hpp"

#include <algorithm>

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

Result<std::vector<std::string>> read_string_array(const JsonValue& value, std::string_view key) {
  std::vector<std::string> out;
  const JsonValue* found = value.find(key);
  if (found == nullptr) {
    return out;
  }
  if (!found->is_array()) {
    return Error(ErrorCode::PersistenceCorrupt, "expected a string array in the decision record");
  }
  if (found->size() > 4096) {
    return Error(ErrorCode::BoundsExceeded, "decision string array exceeds bound");
  }
  for (const auto& entry : found->as_array()) {
    if (!entry.is_string()) {
      return Error(ErrorCode::PersistenceCorrupt, "decision string array entry is not a string");
    }
    out.push_back(entry.as_string());
  }
  return out;
}

}  // namespace

const char* to_string(DecisionOutcome outcome) {
  switch (outcome) {
    case DecisionOutcome::Applied: return "applied";
    case DecisionOutcome::Blocked: return "blocked";
    case DecisionOutcome::Rejected: return "rejected";
    case DecisionOutcome::NoOp: return "no-op";
  }
  return "unknown";
}

std::optional<DecisionOutcome> decision_outcome_from_string(std::string_view text) {
  if (text == "applied") return DecisionOutcome::Applied;
  if (text == "blocked") return DecisionOutcome::Blocked;
  if (text == "rejected") return DecisionOutcome::Rejected;
  if (text == "no-op") return DecisionOutcome::NoOp;
  return std::nullopt;
}

JsonValue Decision::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("action", JsonValue(action));
  value.set("outcome", JsonValue(std::string(drain::to_string(outcome))));
  value.set("drain", JsonValue(drain.value()));
  value.set("set", JsonValue(set.value()));
  value.set("generation", JsonValue(generation.value()));
  value.set("epoch", JsonValue(epoch.value()));
  value.set("incarnation", JsonValue(incarnation.value()));
  value.set("authority", JsonValue(authority.value()));
  value.set("policy_revision", JsonValue(policy_revision.value()));
  value.set("policy_fingerprint", JsonValue(policy_fingerprint));
  value.set("at_ns", JsonValue(static_cast<std::uint64_t>(at.count())));
  JsonValue inputs_json = JsonValue::array();
  for (const auto& entry : inputs) {
    inputs_json.array_ref().push_back(JsonValue(entry));
  }
  value.set("inputs", std::move(inputs_json));
  JsonValue evidence_json = JsonValue::array();
  for (const auto& entry : evidence) {
    evidence_json.array_ref().push_back(JsonValue(entry));
  }
  value.set("evidence", std::move(evidence_json));
  JsonValue rejected_json = JsonValue::array();
  for (const auto& entry : rejected) {
    rejected_json.array_ref().push_back(JsonValue(entry));
  }
  value.set("rejected", std::move(rejected_json));
  JsonValue blockers_json = JsonValue::array();
  for (const auto& blocker : blockers) {
    blockers_json.array_ref().push_back(blocker.to_json());
  }
  value.set("blockers", std::move(blockers_json));
  return value;
}

Result<Decision> Decision::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "decision record is not an object");
  }
  Decision decision;
  decision.action = value.get_string("action");
  if (decision.action.empty()) {
    return Error(ErrorCode::PersistenceCorrupt, "decision record lacks an action");
  }
  const auto outcome = decision_outcome_from_string(value.get_string("outcome"));
  if (!outcome.has_value()) {
    return Error(ErrorCode::PersistenceCorrupt, "decision record has an unknown outcome");
  }
  decision.outcome = *outcome;
  decision.drain = DrainId{read_u64(value, "drain")};
  decision.set = DrainSetId{read_u64(value, "set")};
  decision.generation = Generation{read_u64(value, "generation")};
  decision.epoch = Epoch{read_u64(value, "epoch")};
  decision.incarnation = IncarnationId{read_u64(value, "incarnation")};
  decision.authority = AuthorityId{read_u64(value, "authority")};
  decision.policy_revision = Revision{read_u64(value, "policy_revision")};
  decision.policy_fingerprint = value.get_string("policy_fingerprint");
  decision.at = TimePoint{read_i64(value, "at_ns")};

  auto inputs = read_string_array(value, "inputs");
  if (!inputs.ok()) return inputs.error();
  decision.inputs = *inputs;
  auto evidence = read_string_array(value, "evidence");
  if (!evidence.ok()) return evidence.error();
  decision.evidence = *evidence;
  auto rejected = read_string_array(value, "rejected");
  if (!rejected.ok()) return rejected.error();
  decision.rejected = *rejected;

  if (const JsonValue* blockers = value.find("blockers"); blockers != nullptr && blockers->is_array()) {
    if (blockers->size() > 4096) {
      return Error(ErrorCode::BoundsExceeded, "decision blocker array exceeds bound");
    }
    for (const auto& entry : blockers->as_array()) {
      auto blocker = Blocker::from_json(entry);
      if (!blocker.ok()) {
        return blocker.error();
      }
      decision.blockers.push_back(*blocker);
    }
  }
  std::sort(decision.blockers.begin(), decision.blockers.end());
  return decision;
}

std::string Decision::render() const {
  std::string out;
  out.append("action=");
  out.append(action);
  out.append(" outcome=");
  out.append(drain::to_string(outcome));
  out.append(" drain=");
  out.append(drain::to_string(drain));
  out.append(" generation=");
  out.append(drain::to_string(generation));
  out.append(" epoch=");
  out.append(drain::to_string(epoch));
  out.append(" policy=");
  out.append(drain::to_string(policy_revision));
  out.append("/");
  out.append(policy_fingerprint);
  out.append(" at=");
  out.append(describe_time(at));
  for (const auto& entry : inputs) {
    out.append("\n  input: ");
    out.append(entry);
  }
  for (const auto& entry : evidence) {
    out.append("\n  evidence: ");
    out.append(entry);
  }
  for (const auto& entry : rejected) {
    out.append("\n  rejected: ");
    out.append(entry);
  }
  for (const auto& blocker : blockers) {
    out.append("\n  blocker: ");
    out.append(blocker.render());
  }
  return out;
}

}  // namespace drain
