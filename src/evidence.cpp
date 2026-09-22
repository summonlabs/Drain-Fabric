#include "drain/evidence.hpp"

#include <algorithm>

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

const char* to_string(EvidenceKind kind) {
  switch (kind) {
    case EvidenceKind::AdmissionClosed: return "admission-closed";
    case EvidenceKind::ObligationEvacuated: return "obligation-evacuated";
    case EvidenceKind::ObligationReleased: return "obligation-released";
    case EvidenceKind::FlowQuiesced: return "flow-quiesced";
    case EvidenceKind::PathAvailable: return "path-available";
    case EvidenceKind::PathLost: return "path-lost";
    case EvidenceKind::CapacityAvailable: return "capacity-available";
    case EvidenceKind::ResourceRemoved: return "resource-removed";
    case EvidenceKind::ResourceRestored: return "resource-restored";
  }
  return "unknown";
}

std::optional<EvidenceKind> evidence_kind_from_string(std::string_view text) {
  if (text == "admission-closed") return EvidenceKind::AdmissionClosed;
  if (text == "obligation-evacuated") return EvidenceKind::ObligationEvacuated;
  if (text == "obligation-released") return EvidenceKind::ObligationReleased;
  if (text == "flow-quiesced") return EvidenceKind::FlowQuiesced;
  if (text == "path-available") return EvidenceKind::PathAvailable;
  if (text == "path-lost") return EvidenceKind::PathLost;
  if (text == "capacity-available") return EvidenceKind::CapacityAvailable;
  if (text == "resource-removed") return EvidenceKind::ResourceRemoved;
  if (text == "resource-restored") return EvidenceKind::ResourceRestored;
  return std::nullopt;
}

std::string EvidenceKey::label() const {
  std::string out = drain::to_string(kind);
  out.append(" drain=");
  out.append(drain::to_string(drain));
  out.append(" obligation=");
  out.append(drain::to_string(obligation));
  out.append(" resource=");
  out.append(resource.empty() ? std::string("-") : resource.str());
  return out;
}

JsonValue Evidence::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("seq", JsonValue(seq.value()));
  value.set("kind", JsonValue(std::string(drain::to_string(key.kind))));
  value.set("drain", JsonValue(key.drain.value()));
  value.set("obligation", JsonValue(key.obligation.value()));
  value.set("resource", JsonValue(key.resource.str()));
  value.set("generation", JsonValue(generation.value()));
  value.set("epoch", JsonValue(epoch.value()));
  value.set("producer", JsonValue(producer.value()));
  value.set("authority", JsonValue(authority.value()));
  value.set("observed_at_ns", JsonValue(static_cast<std::uint64_t>(observed_at.count())));
  value.set("expires_at_ns", JsonValue(static_cast<std::uint64_t>(expires_at.count())));
  value.set("healthy", JsonValue(healthy));
  value.set("quantity", JsonValue(quantity));
  value.set("detail", JsonValue(detail));
  value.set("attested", JsonValue(attested));
  return value;
}

Result<Evidence> Evidence::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "evidence entry is not an object");
  }
  const auto kind = evidence_kind_from_string(value.get_string("kind"));
  if (!kind.has_value()) {
    return Error(ErrorCode::PersistenceCorrupt, "evidence entry has an unknown kind");
  }
  Evidence evidence;
  evidence.seq = EvidenceSeq{read_u64(value, "seq")};
  if (evidence.seq.is_zero()) {
    return Error(ErrorCode::PersistenceCorrupt, "evidence entry has a reserved zero sequence");
  }
  evidence.key.kind = *kind;
  evidence.key.drain = DrainId{read_u64(value, "drain")};
  evidence.key.obligation = ObligationId{read_u64(value, "obligation")};
  const std::string resource = value.get_string("resource");
  if (!resource.empty()) {
    const auto parsed = ResourceId::parse(resource);
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "evidence entry has a malformed resource id");
    }
    evidence.key.resource = *parsed;
  }
  evidence.generation = Generation{read_u64(value, "generation")};
  evidence.epoch = Epoch{read_u64(value, "epoch")};
  evidence.producer = IncarnationId{read_u64(value, "producer")};
  evidence.authority = AuthorityId{read_u64(value, "authority")};
  evidence.observed_at = TimePoint{read_i64(value, "observed_at_ns")};
  evidence.expires_at = TimePoint{read_i64(value, "expires_at_ns")};
  evidence.healthy = value.get_bool("healthy", false);
  evidence.quantity = read_u64(value, "quantity");
  evidence.detail = value.get_string("detail");
  // Attestation is never restored from disk: a recovered record is quarantined
  // until the live incarnation re-attests it.
  evidence.attested = false;
  return evidence;
}

Result<Evidence> EvidenceLedger::record(Evidence draft, const EvidenceFence& fence,
                                        bool require_live_incarnation) {
  if (draft.observed_at > saturating_add(fence.now, fence.allowed_skew)) {
    return Error(ErrorCode::InvalidArgument,
                 "evidence observed beyond the tolerated clock skew is rejected");
  }
  if (draft.epoch != fence.live_epoch) {
    return Error(ErrorCode::StaleEpoch, "evidence produced under a superseded authority epoch");
  }
  if (require_live_incarnation && draft.producer != fence.live_incarnation) {
    return Error(ErrorCode::StaleIncarnation,
                 "evidence produced by incarnation " + drain::to_string(draft.producer) +
                     " cannot be accepted by incarnation " + drain::to_string(fence.live_incarnation));
  }
  const auto it = latest_.find(draft.key);
  if (it != latest_.end() && draft.seq <= it->second) {
    return Error(ErrorCode::DuplicateIdentity,
                 "evidence sequence " + drain::to_string(draft.seq) + " is not newer than " +
                     drain::to_string(it->second) + " for " + draft.key.label());
  }
  if (records_.size() >= max_records_ && !retire_one_for(draft.key)) {
    return Error(ErrorCode::BoundsExceeded, "evidence ledger is at capacity");
  }
  draft.attested = true;
  const EvidenceSeq seq = draft.seq;
  const EvidenceRecordKey record_key{draft.key, seq};
  if (!records_.emplace(record_key, draft).second) {
    return Error(ErrorCode::DuplicateIdentity,
                 "this stream already holds a record with sequence " + drain::to_string(seq));
  }
  latest_[draft.key] = seq;
  if (global_high_water_ < seq) {
    global_high_water_ = seq;
  }
  return draft;
}

void EvidenceLedger::quarantine_all() {
  for (auto& [key, evidence] : records_) {
    (void)key;
    evidence.attested = false;
  }
}

Status EvidenceLedger::attest(const EvidenceKey& key, EvidenceSeq seq, const EvidenceFence& fence) {
  const auto it = records_.find(EvidenceRecordKey{key, seq});
  if (it == records_.end()) {
    return fail(ErrorCode::NotFound, "no evidence record for that stream and sequence");
  }
  if (it->second.epoch != fence.live_epoch) {
    return fail(ErrorCode::StaleEpoch, "evidence record belongs to a superseded epoch");
  }
  if (it->second.producer != fence.live_incarnation) {
    return fail(ErrorCode::StaleIncarnation,
                "evidence record was produced by a previous incarnation and cannot be re-attested");
  }
  it->second.attested = true;
  return ok_status();
}

const Evidence* EvidenceLedger::latest(const EvidenceKey& key) const {
  const auto it = latest_.find(key);
  if (it == latest_.end()) {
    return nullptr;
  }
  const auto record = records_.find(EvidenceRecordKey{key, it->second});
  return record == records_.end() ? nullptr : &record->second;
}

const Evidence* EvidenceLedger::find(const EvidenceKey& key, EvidenceSeq seq) const {
  const auto it = records_.find(EvidenceRecordKey{key, seq});
  return it == records_.end() ? nullptr : &it->second;
}

bool EvidenceLedger::is_fresh(const Evidence& evidence, const EvidenceFence& fence,
                              Duration freshness) const {
  if (!evidence.attested) {
    return false;
  }
  if (evidence.epoch != fence.live_epoch) {
    return false;
  }
  if (evidence.observed_at > saturating_add(fence.now, fence.allowed_skew)) {
    return false;
  }
  if (freshness.count() <= 0) {
    return false;
  }
  return fence.now <= saturating_add(evidence.observed_at, freshness);
}

const Evidence* EvidenceLedger::fresh(const EvidenceKey& key, const EvidenceFence& fence,
                                     Duration freshness) const {
  const Evidence* candidate = latest(key);
  if (candidate == nullptr) {
    return nullptr;
  }
  return is_fresh(*candidate, fence, freshness) ? candidate : nullptr;
}

EvidenceSeq EvidenceLedger::high_water(const EvidenceKey& key) const {
  const auto it = latest_.find(key);
  return it == latest_.end() ? EvidenceSeq{} : it->second;
}

std::uint64_t EvidenceLedger::digest() const {
  Digest64 digest;
  for (const auto& [record_key, evidence] : records_) {
    (void)record_key;
    digest.update_u64(evidence.seq.value());
    digest.update_tagged("key", evidence.key.label());
    digest.update_u64(evidence.generation.value());
    digest.update_u64(evidence.observed_at.count() > 0
                          ? static_cast<std::uint64_t>(evidence.observed_at.count())
                          : 0);
    digest.update_bool(evidence.healthy);
    digest.update_u64(evidence.quantity);
    digest.update_bool(evidence.attested);
  }
  return digest.value();
}

bool EvidenceLedger::retire_one_for(const EvidenceKey& incoming) {
  // Records are ordered by (stream, sequence), so the first match on the
  // incoming stream is its oldest record -- exactly the one this write
  // supersedes.
  for (auto it = records_.begin(); it != records_.end(); ++it) {
    if (it->second.key == incoming) {
      records_.erase(it);
      return true;
    }
  }
  for (auto it = records_.begin(); it != records_.end(); ++it) {
    const auto latest = latest_.find(it->second.key);
    if (latest != latest_.end() && latest->second != it->second.seq) {
      records_.erase(it);
      return true;
    }
  }
  return false;
}

JsonValue EvidenceLedger::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("global_high_water", JsonValue(global_high_water_.value()));
  JsonValue items = JsonValue::array();
  for (const auto& [record_key, evidence] : records_) {
    (void)record_key;
    items.array_ref().push_back(evidence.to_json());
  }
  value.set("records", std::move(items));
  return value;
}

Status EvidenceLedger::load_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return fail(ErrorCode::PersistenceCorrupt, "evidence ledger payload is not an object");
  }
  const JsonValue* items = value.find("records");
  if (items == nullptr || !items->is_array()) {
    return fail(ErrorCode::PersistenceCorrupt, "evidence ledger payload lacks a records array");
  }
  if (items->size() > max_records_) {
    return fail(ErrorCode::BoundsExceeded, "snapshot evidence count exceeds the configured bound");
  }
  std::map<EvidenceRecordKey, Evidence> loaded;
  std::map<EvidenceKey, EvidenceSeq> latest;
  EvidenceSeq high_water{};
  for (const auto& entry : items->as_array()) {
    auto evidence = Evidence::from_json(entry);
    if (!evidence.ok()) {
      return fail(ErrorCode::PersistenceCorrupt, "evidence entry rejected: " + evidence.error().message());
    }
    const auto key = evidence->key;
    const auto seq = evidence->seq;
    const auto existing = latest.find(key);
    if (existing != latest.end() && seq <= existing->second) {
      return fail(ErrorCode::PersistenceCorrupt, "evidence sequence is not monotonic within a stream");
    }
    if (!loaded.emplace(EvidenceRecordKey{key, seq}, *evidence).second) {
      return fail(ErrorCode::PersistenceCorrupt, "duplicate evidence record in snapshot");
    }
    latest[key] = seq;
    if (high_water < seq) {
      high_water = seq;
    }
  }
  const std::uint64_t declared = read_u64(value, "global_high_water");
  if (declared < high_water.value()) {
    return fail(ErrorCode::PersistenceCorrupt, "evidence high-water mark is behind the stored records");
  }
  records_ = std::move(loaded);
  latest_ = std::move(latest);
  global_high_water_ = EvidenceSeq{declared};
  return ok_status();
}

void EvidenceLedger::clear() {
  records_.clear();
  latest_.clear();
  global_high_water_ = EvidenceSeq{};
}

}  // namespace drain
