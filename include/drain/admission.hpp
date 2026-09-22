#pragma once

// Drain Fabric -- admission fence.
//
// The fence is the authoritative boundary between "new obligations may still
// bind to this target" and "they may not". It is keyed by drain target and every
// closure carries the generation that closed it. Reopening is a separate,
// generation-advancing operation, so a caller that holds a superseded generation
// is rejected with a fencing error instead of silently binding new work to a
// resource that is being drained.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "drain/clock.hpp"
#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"

namespace drain {

inline constexpr std::size_t kMaxFencedTargets = 65536;

struct FenceEntry {
  bool closed{false};
  Generation generation{};
  DrainId drain{};
  TimePoint closed_at{0};
  TimePoint opened_at{0};
  std::string reason{};

  JsonValue to_json() const;
  static Result<FenceEntry> from_json(const JsonValue& value);
};

class AdmissionFence {
 public:
  /// Closes admission for a target, advancing its generation. Returns the
  /// generation that now governs the target.
  Result<Generation> close(const DrainTarget& target, DrainId drain, TimePoint at, std::string reason);

  /// Reopens admission under a strictly newer generation. Rejected when the
  /// target is not currently closed.
  Result<Generation> reopen(const DrainTarget& target, DrainId drain, TimePoint at, std::string reason);

  bool is_closed(const DrainTarget& target) const;
  const FenceEntry* entry(const DrainTarget& target) const;

  /// Current generation for a target. Zero means the target has never been
  /// fenced.
  Generation generation(const DrainTarget& target) const;

  /// Admission decision for a caller that presents the generation it believes
  /// is current.
  Status check_admission(const DrainTarget& target, Generation presented) const;

  std::vector<std::pair<DrainTarget, FenceEntry>> entries() const;
  std::size_t closed_count() const;

  JsonValue to_json() const;
  Status load_from_json(const JsonValue& value);

  void clear();

 private:
  std::map<DrainTarget, FenceEntry> entries_{};
};

}  // namespace drain
