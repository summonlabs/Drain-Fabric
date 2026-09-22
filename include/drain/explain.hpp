#pragma once

// Drain Fabric -- deterministic explanations and accounting audits.
//
// An explanation answers "why is this drain where it is" from authoritative
// state only: the blocker set, the obligations that still depend on the target,
// and the decisions that produced the current state. An accounting report
// answers "does the whole engine add up": every completed drain must show zero
// remaining protected dependencies, and every fence must be accounted for by an
// active or settled drain.

#include <cstdint>
#include <string>
#include <vector>

#include "drain/decision.hpp"
#include "drain/export.hpp"
#include "drain/json.hpp"
#include "drain/lifecycle.hpp"
#include "drain/obligation.hpp"

namespace drain {

/// A policy exception: extends the grace window for one obligation (or for every
/// outstanding obligation when the obligation id is zero) and resets the
/// automatic evacuation retry budget. Exceptions are bounded by policy.
struct ExceptionGrant {
  DrainId drain{};
  ObligationId obligation{};
  Duration extension{0};
  AuthorityId authority{};
  TimePoint granted_at{0};
  std::uint32_t sequence{0};

  JsonValue to_json() const;
  static Result<ExceptionGrant> from_json(const JsonValue& value);
};

/// Counters describing what the engine has done. Used by the CLI and by the
/// concurrency tests to prove that cancellation stops work.
struct EngineStats {
  std::uint64_t advances{0};
  std::uint64_t transitions{0};
  std::uint64_t evacuation_requests{0};
  std::uint64_t restoration_requests{0};
  std::uint64_t reopen_requests{0};
  std::uint64_t rejected_reports{0};
  std::uint64_t rejected_admissions{0};
  std::uint64_t rejected_evidence{0};
  std::uint64_t blocked_evaluations{0};
  std::uint64_t sink_failures{0};
  std::uint64_t completion_proofs{0};

  JsonValue to_json() const;
  std::string render() const;
};

struct Explanation {
  bool found{false};
  DrainRecord drain{};
  std::vector<Blocker> blockers{};
  std::vector<Obligation> dependents{};
  std::vector<Obligation> outstanding{};
  std::vector<Decision> history{};
  std::string summary{};

  JsonValue to_json() const;
  std::string render() const;
};

struct AccountingReport {
  std::size_t drains_total{0};
  std::size_t drains_active{0};
  std::size_t drains_blocked{0};
  std::size_t drains_drained{0};
  std::size_t drains_cancelled{0};
  std::size_t drains_failed{0};
  std::size_t drains_restoring{0};
  std::size_t drains_restored{0};
  std::size_t obligations_total{0};
  std::size_t obligations_outstanding_protected{0};
  std::size_t obligations_retired{0};
  std::size_t obligations_reappeared{0};
  std::size_t fences_closed{0};
  std::size_t evidence_records{0};
  std::size_t drain_sets{0};
  std::uint64_t authoritative_digest{0};
  std::uint64_t topology_digest{0};
  std::uint64_t obligation_digest{0};
  std::uint64_t evidence_digest{0};
  std::vector<std::string> violations{};

  bool clean() const noexcept { return violations.empty(); }
  JsonValue to_json() const;
  std::string render() const;
};

}  // namespace drain
