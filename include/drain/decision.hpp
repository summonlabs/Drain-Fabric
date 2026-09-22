#pragma once

// Drain Fabric -- deterministic decision records.
//
// Every state change the engine applies, and every change it refuses to apply,
// produces a Decision. A Decision names the action, the inputs and evidence that
// were consulted, the governing policy revision and fingerprint, the generation
// and authority under which it was taken, the alternatives that were considered
// and rejected, and the blockers that prevented progress. Two runs over the same
// inputs must produce byte-identical decisions.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "drain/clock.hpp"
#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/lifecycle.hpp"
#include "drain/result.hpp"

namespace drain {

enum class DecisionOutcome : std::uint8_t {
  Applied = 0,
  Blocked = 1,
  Rejected = 2,
  NoOp = 3,
};

DRAIN_API const char* to_string(DecisionOutcome outcome);
DRAIN_API std::optional<DecisionOutcome> decision_outcome_from_string(std::string_view text);

struct Decision {
  std::string action{};
  DecisionOutcome outcome{DecisionOutcome::NoOp};
  DrainId drain{};
  DrainSetId set{};
  Generation generation{};
  Epoch epoch{};
  IncarnationId incarnation{};
  AuthorityId authority{};
  Revision policy_revision{};
  std::string policy_fingerprint{};
  TimePoint at{0};
  std::vector<std::string> inputs{};
  std::vector<std::string> evidence{};
  std::vector<std::string> rejected{};
  std::vector<Blocker> blockers{};

  JsonValue to_json() const;
  static Result<Decision> from_json(const JsonValue& value);

  /// Canonical multi-line rendering used by the explain command.
  std::string render() const;
};

}  // namespace drain
