#pragma once

// Drain Fabric -- evidence ledger.
//
// Evidence is the only currency that can prove a drain step. Every record is
// bound to the incarnation that produced it, the authority epoch under which it
// was produced, the drain generation it belongs to, and the instant it was
// observed. A record that cannot be re-attested by the live incarnation is
// quarantined rather than reused: deserializing a record after a restart never
// makes it current again.

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

enum class EvidenceKind : std::uint8_t {
  AdmissionClosed = 0,
  ObligationEvacuated = 1,
  ObligationReleased = 2,
  FlowQuiesced = 3,
  PathAvailable = 4,
  PathLost = 5,
  CapacityAvailable = 6,
  ResourceRemoved = 7,
  ResourceRestored = 8,
};

DRAIN_API const char* to_string(EvidenceKind kind);
DRAIN_API std::optional<EvidenceKind> evidence_kind_from_string(std::string_view text);

/// Identity of an evidence stream. A stream is monotonic: each new record for a
/// key must carry a strictly larger sequence number, so replays and reordered
/// duplicates are rejected.
struct EvidenceKey {
  EvidenceKind kind{EvidenceKind::AdmissionClosed};
  DrainId drain{};
  ObligationId obligation{};
  ResourceId resource{};

  friend bool operator==(const EvidenceKey&, const EvidenceKey&) noexcept = default;
  friend bool operator<(const EvidenceKey& a, const EvidenceKey& b) noexcept {
    if (a.kind != b.kind) {
      return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    }
    if (a.drain != b.drain) {
      return a.drain < b.drain;
    }
    if (a.obligation != b.obligation) {
      return a.obligation < b.obligation;
    }
    return a.resource < b.resource;
  }

  std::string label() const;
};

/// One observation. Only the fields that matter for the decision are kept; the
/// payload is deliberately small so the ledger stays bounded.
struct Evidence {
  EvidenceSeq seq{};
  EvidenceKey key{};
  Generation generation{};
  Epoch epoch{};
  IncarnationId producer{};
  AuthorityId authority{};
  TimePoint observed_at{0};
  TimePoint expires_at{0};
  bool healthy{false};
  std::uint64_t quantity{0};
  std::string detail{};
  /// False while the record is quarantined. Loaded records start quarantined;
  /// only a re-attestation by the live incarnation clears it.
  bool attested{false};

  JsonValue to_json() const;
  static Result<Evidence> from_json(const JsonValue& value);
};

/// Storage key for one record. Sequences are monotonic *per key*, so the ledger
/// must be keyed by both: two different streams may legitimately use the same
/// sequence number, and keying by sequence alone silently aliases them.
struct EvidenceRecordKey {
  EvidenceKey key{};
  EvidenceSeq seq{};

  friend bool operator<(const EvidenceRecordKey& a, const EvidenceRecordKey& b) noexcept {
    if (a.key < b.key) {
      return true;
    }
    if (b.key < a.key) {
      return false;
    }
    return a.seq < b.seq;
  }
};

/// Fences for evidence acceptance. The engine supplies the live incarnation and
/// epoch; the ledger enforces monotonic sequences and time sanity.
struct EvidenceFence {
  IncarnationId live_incarnation{};
  Epoch live_epoch{};
  TimePoint now{0};
  /// Bounded tolerance for an observation that is slightly ahead of the local
  /// clock. Zero means observations must never be in the future.
  Duration allowed_skew{0};
};

class EvidenceLedger {
 public:
  explicit EvidenceLedger(std::size_t max_records = 200000) : max_records_(max_records) {}

  /// Records an observation. Rejected when:
  ///  - the observed instant is in the future (clock skew or forgery),
  ///  - the sequence is not strictly greater than the newest sequence for the
  ///    key (replay, reordered duplicate, or a rewind),
  ///  - the record was produced by a different incarnation while the fence
  ///    requires the live incarnation,
  ///  - the epoch does not match the live epoch,
  ///  - the ledger is at capacity and no superseded record can be retired.
  Result<Evidence> record(Evidence draft, const EvidenceFence& fence, bool require_live_incarnation);

  /// Quarantines every record. Called on recovery so that nothing loaded from
  /// disk is trusted until the live incarnation re-attests it.
  void quarantine_all();

  /// Re-attests a quarantined record. Rejected when the epoch or incarnation in
  /// the fence does not match the record.
  Status attest(const EvidenceKey& key, EvidenceSeq seq, const EvidenceFence& fence);

  const Evidence* latest(const EvidenceKey& key) const;

  /// Looks up one record of one stream.
  const Evidence* find(const EvidenceKey& key, EvidenceSeq seq) const;

  /// Fresh means: attested, epoch matches, observed not in the future, and not
  /// older than the freshness window at the given instant.
  bool is_fresh(const Evidence& evidence, const EvidenceFence& fence, Duration freshness) const;

  /// Convenience: fresh evidence for a key, if any.
  const Evidence* fresh(const EvidenceKey& key, const EvidenceFence& fence, Duration freshness) const;

  /// Every record, ordered by stream and then by sequence.
  std::vector<Evidence> all() const {
    std::vector<Evidence> out;
    out.reserve(records_.size());
    for (const auto& [key, evidence] : records_) {
      (void)key;
      out.push_back(evidence);
    }
    return out;
  }

  std::size_t size() const noexcept { return records_.size(); }
  std::size_t max_records() const noexcept { return max_records_; }
  void set_max_records(std::size_t value) noexcept { max_records_ = value; }

  /// Highest sequence ever accepted for a key, or zero.
  EvidenceSeq high_water(const EvidenceKey& key) const;

  /// Highest sequence ever accepted across all keys.
  EvidenceSeq global_high_water() const noexcept { return global_high_water_; }

  /// Deterministic digest of the attested, unexpired evidence for a key set.
  std::uint64_t digest() const;

  JsonValue to_json() const;
  Status load_from_json(const JsonValue& value);

  void clear();

 private:
  /// Frees one slot for an incoming record. A record this write supersedes is
  /// preferred; otherwise any record already superseded by a newer one on its
  /// own stream is retired. Returns false when nothing can be freed.
  bool retire_one_for(const EvidenceKey& incoming);

  std::map<EvidenceRecordKey, Evidence> records_{};
  std::map<EvidenceKey, EvidenceSeq> latest_{};
  std::size_t max_records_{200000};
  EvidenceSeq global_high_water_{};
};

}  // namespace drain
