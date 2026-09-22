#include "drain/admission.hpp"

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

JsonValue FenceEntry::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("closed", JsonValue(closed));
  value.set("generation", JsonValue(generation.value()));
  value.set("drain", JsonValue(drain.value()));
  value.set("closed_at_ns", JsonValue(static_cast<std::uint64_t>(closed_at.count())));
  value.set("opened_at_ns", JsonValue(static_cast<std::uint64_t>(opened_at.count())));
  value.set("reason", JsonValue(reason));
  return value;
}

Result<FenceEntry> FenceEntry::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "fence entry is not an object");
  }
  FenceEntry entry;
  entry.closed = value.get_bool("closed", false);
  entry.generation = Generation{read_u64(value, "generation")};
  entry.drain = DrainId{read_u64(value, "drain")};
  entry.closed_at = TimePoint{read_i64(value, "closed_at_ns")};
  entry.opened_at = TimePoint{read_i64(value, "opened_at_ns")};
  entry.reason = value.get_string("reason");
  if (entry.generation.is_zero()) {
    return Error(ErrorCode::PersistenceCorrupt, "fence entry has a reserved zero generation");
  }
  if (entry.closed && entry.drain.is_zero()) {
    return Error(ErrorCode::PersistenceCorrupt, "closed fence entry lacks a drain id");
  }
  return entry;
}

Result<Generation> AdmissionFence::close(const DrainTarget& target, DrainId drain, TimePoint at,
                                         std::string reason) {
  if (drain.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a fence closure must name the drain that closed it");
  }
  auto& entry = entries_[target];
  const auto next = increment_checked(entry.generation.value());
  if (!next.has_value()) {
    entries_.erase(target);
    return Error(ErrorCode::BoundsExceeded, "admission generation space exhausted for this target");
  }
  if (entries_.size() > kMaxFencedTargets) {
    entries_.erase(target);
    return Error(ErrorCode::BoundsExceeded, "admission fence table is at capacity");
  }
  entry.closed = true;
  entry.generation = Generation{*next};
  entry.drain = drain;
  entry.closed_at = at;
  entry.reason = std::move(reason);
  return entry.generation;
}

Result<Generation> AdmissionFence::reopen(const DrainTarget& target, DrainId drain, TimePoint at,
                                          std::string reason) {
  const auto it = entries_.find(target);
  if (it == entries_.end() || !it->second.closed) {
    return Error(ErrorCode::IllegalTransition, "admission for this target is not closed");
  }
  if (drain.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "a fence reopen must name the drain that owns the closure");
  }
  const auto next = increment_checked(it->second.generation.value());
  if (!next.has_value()) {
    return Error(ErrorCode::BoundsExceeded, "admission generation space exhausted for this target");
  }
  it->second.closed = false;
  it->second.generation = Generation{*next};
  it->second.drain = drain;
  it->second.opened_at = at;
  it->second.reason = std::move(reason);
  return it->second.generation;
}

bool AdmissionFence::is_closed(const DrainTarget& target) const {
  const auto it = entries_.find(target);
  return it != entries_.end() && it->second.closed;
}

const FenceEntry* AdmissionFence::entry(const DrainTarget& target) const {
  const auto it = entries_.find(target);
  return it == entries_.end() ? nullptr : &it->second;
}

Generation AdmissionFence::generation(const DrainTarget& target) const {
  const auto it = entries_.find(target);
  return it == entries_.end() ? Generation{} : it->second.generation;
}

Status AdmissionFence::check_admission(const DrainTarget& target, Generation presented) const {
  const auto it = entries_.find(target);
  if (it == entries_.end()) {
    if (!presented.is_zero()) {
      return fail(ErrorCode::StaleGeneration,
                  "target " + target.to_string() + " has never been fenced, so generation " +
                      drain::to_string(presented) + " cannot be current");
    }
    return ok_status();
  }
  if (presented != it->second.generation) {
    return fail(ErrorCode::StaleGeneration, "target " + target.to_string() + " is at generation " +
                                                drain::to_string(it->second.generation) + " but the caller presented " +
                                                drain::to_string(presented));
  }
  if (it->second.closed) {
    return fail(ErrorCode::AdmissionClosed, "admission for " + target.to_string() +
                                                " is closed by drain " + drain::to_string(it->second.drain));
  }
  return ok_status();
}

std::vector<std::pair<DrainTarget, FenceEntry>> AdmissionFence::entries() const {
  std::vector<std::pair<DrainTarget, FenceEntry>> out;
  out.reserve(entries_.size());
  for (const auto& [target, entry] : entries_) {
    out.emplace_back(target, entry);
  }
  return out;
}

std::size_t AdmissionFence::closed_count() const {
  std::size_t count = 0;
  for (const auto& [target, entry] : entries_) {
    (void)target;
    if (entry.closed) {
      ++count;
    }
  }
  return count;
}

JsonValue AdmissionFence::to_json() const {
  JsonValue value = JsonValue::object();
  JsonValue items = JsonValue::array();
  for (const auto& [target, entry] : entries_) {
    JsonValue item = JsonValue::object();
    item.set("target", drain::to_json(target));
    item.set("entry", entry.to_json());
    items.array_ref().push_back(std::move(item));
  }
  value.set("entries", std::move(items));
  return value;
}

Status AdmissionFence::load_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return fail(ErrorCode::PersistenceCorrupt, "admission fence payload is not an object");
  }
  const JsonValue* items = value.find("entries");
  if (items == nullptr || !items->is_array()) {
    return fail(ErrorCode::PersistenceCorrupt, "admission fence payload lacks an entries array");
  }
  if (items->size() > kMaxFencedTargets) {
    return fail(ErrorCode::BoundsExceeded, "fence entry count exceeds bound");
  }
  std::map<DrainTarget, FenceEntry> staged;
  for (const auto& item : items->as_array()) {
    const JsonValue* target_value = item.find("target");
    const JsonValue* entry_value = item.find("entry");
    if (target_value == nullptr || entry_value == nullptr) {
      return fail(ErrorCode::PersistenceCorrupt, "fence entry lacks target or entry payload");
    }
    auto target = drain_target_from_json(*target_value);
    if (!target.ok()) {
      return fail(ErrorCode::PersistenceCorrupt, "fence entry target rejected");
    }
    auto entry = FenceEntry::from_json(*entry_value);
    if (!entry.ok()) {
      return fail(ErrorCode::PersistenceCorrupt, "fence entry rejected: " + entry.error().message());
    }
    if (!staged.emplace(*target, *entry).second) {
      return fail(ErrorCode::PersistenceCorrupt, "duplicate fence entry for a target");
    }
  }
  entries_ = std::move(staged);
  return ok_status();
}

void AdmissionFence::clear() { entries_.clear(); }

}  // namespace drain
