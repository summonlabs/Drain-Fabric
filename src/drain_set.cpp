#include "drain/drain_set.hpp"

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

JsonValue DrainSet::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id.value()));
  value.set("reason", JsonValue(reason));
  value.set("requested_at_ns", JsonValue(static_cast<std::uint64_t>(requested_at.count())));
  value.set("revision", JsonValue(revision.value()));
  value.set("authority", JsonValue(authority.value()));
  value.set("nonce", JsonValue(nonce.value()));
  JsonValue targets_json = JsonValue::array();
  for (const auto& target : targets) {
    targets_json.array_ref().push_back(drain::to_json(target));
  }
  value.set("targets", std::move(targets_json));
  JsonValue members_json = JsonValue::array();
  for (const auto& member : members) {
    members_json.array_ref().push_back(JsonValue(member.value()));
  }
  value.set("members", std::move(members_json));
  return value;
}

Result<DrainSet> DrainSet::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "drain set is not an object");
  }
  DrainSet set;
  set.id = DrainSetId{read_u64(value, "id")};
  if (set.id.is_zero()) {
    return Error(ErrorCode::PersistenceCorrupt, "drain set has a reserved zero id");
  }
  set.reason = value.get_string("reason");
  set.requested_at = TimePoint{read_i64(value, "requested_at_ns")};
  set.revision = Revision{read_u64(value, "revision", 1)};
  set.authority = AuthorityId{read_u64(value, "authority")};
  set.nonce = RequestNonce{read_u64(value, "nonce")};

  const JsonValue* targets = value.find("targets");
  if (targets == nullptr || !targets->is_array() || targets->size() == 0) {
    return Error(ErrorCode::PersistenceCorrupt, "drain set lacks targets");
  }
  if (targets->size() > kMaxTargetsPerRequest) {
    return Error(ErrorCode::BoundsExceeded, "drain set target count exceeds bound");
  }
  for (const auto& entry : targets->as_array()) {
    auto target = drain_target_from_json(entry);
    if (!target.ok()) {
      return target.error();
    }
    set.targets.push_back(*target);
  }
  const JsonValue* members = value.find("members");
  if (members != nullptr && members->is_array()) {
    if (members->size() > kMaxTargetsPerRequest) {
      return Error(ErrorCode::BoundsExceeded, "drain set member count exceeds bound");
    }
    for (const auto& entry : members->as_array()) {
      set.members.push_back(DrainId{entry.as_uint()});
    }
  }
  return set;
}

Result<std::vector<DrainTarget>> normalize_targets(std::vector<DrainTarget> targets, std::size_t max_targets) {
  if (targets.empty()) {
    return Error(ErrorCode::InvalidArgument, "a drain request must name at least one target");
  }
  if (targets.size() > max_targets) {
    return Error(ErrorCode::BoundsExceeded, "a drain request exceeds the policy target limit");
  }
  for (const auto& target : targets) {
    if (target.id().empty()) {
      return Error(ErrorCode::MalformedIdentity, "drain target resource id must not be empty");
    }
  }
  std::sort(targets.begin(), targets.end(), target_order_less);
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

}  // namespace drain
