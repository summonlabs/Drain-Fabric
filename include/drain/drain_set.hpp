#pragma once

// Drain Fabric -- multi-resource drain sets.
//
// A set is an ordered batch of drains that must be admitted together. Order is
// derived from the target order (failure domain first, then kind, then
// identity), never from caller order, so a set produces the same execution
// sequence no matter how the request listed its targets.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "drain/clock.hpp"
#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"

namespace drain {

struct DrainSet {
  DrainSetId id{};
  std::string reason{};
  TimePoint requested_at{0};
  Revision revision{Revision{1}};
  AuthorityId authority{};
  RequestNonce nonce{};
  std::vector<DrainTarget> targets{};
  std::vector<DrainId> members{};

  JsonValue to_json() const;
  static Result<DrainSet> from_json(const JsonValue& value);
};

/// Sorts targets into the canonical execution order and removes duplicates.
/// Returns an error when the request is empty or larger than max_targets.
DRAIN_API Result<std::vector<DrainTarget>> normalize_targets(std::vector<DrainTarget> targets,
                                                             std::size_t max_targets);

}  // namespace drain
