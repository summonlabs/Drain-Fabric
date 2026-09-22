#pragma once

// Drain Fabric -- shared test fixtures.
//
// The fixtures here are test doubles for *adjacent runtimes*, not for Drain
// Fabric itself: they record the evacuation requests Drain Fabric issues and can
// answer them the way a real reroute/migration controller would, by recording
// evidence and reporting the obligation state back through the public API. The
// sink deliberately calls back into the engine, which is what proves that the
// engine never invokes a hook while holding its mutex.

#include <cstdint>
#include <string>
#include <vector>

#include "drain/engine.hpp"
#include "drain/obligation.hpp"
#include "drain/topology.hpp"
#include "test.hpp"

namespace drain_test {

/// Deterministic xoshiro256** generator seeded through splitmix64. Every
/// randomized test prints its seed on failure so a run is reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) {
    std::uint64_t mixer = seed;
    for (std::size_t index = 0; index < 4; ++index) {
      state_[index] = splitmix(mixer);
      mixer = state_[index];
    }
  }

  std::uint64_t next_u64() {
    const std::uint64_t result = rotate(state_[1] * 5u, 7) * 9u;
    const std::uint64_t temporary = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= temporary;
    state_[3] = rotate(state_[3], 45);
    return result;
  }

  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next_u64() % bound; }

  std::size_t index(std::size_t bound) { return bound == 0 ? 0 : static_cast<std::size_t>(below(bound)); }

  bool chance(std::uint32_t numerator, std::uint32_t denominator) {
    if (denominator == 0) {
      return false;
    }
    return below(denominator) < numerator;
  }

  std::uint64_t seed() const noexcept { return seed_; }
  void set_seed(std::uint64_t seed) {
    seed_ = seed;
    std::uint64_t mixer = seed;
    for (std::size_t index = 0; index < 4; ++index) {
      state_[index] = splitmix(mixer);
      mixer = state_[index];
    }
  }

 private:
  static std::uint64_t rotate(std::uint64_t value, int bits) {
    return (value << bits) | (value >> (64 - bits));
  }

  static std::uint64_t splitmix(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  std::uint64_t state_[4]{};
  std::uint64_t seed_{0};
};

inline drain::DrainTarget make_target(drain::TargetKind kind, const std::string& id, const std::string& domain) {
  const auto resource = drain::ResourceId::parse(id);
  DRAIN_CHECK_MSG(resource.has_value(), "test target id must be a valid slug");
  drain::DomainId parsed_domain;
  if (!domain.empty()) {
    const auto parsed = drain::DomainId::parse(domain);
    DRAIN_CHECK_MSG(parsed.has_value(), "test domain id must be a valid slug");
    parsed_domain = *parsed;
  }
  return drain::DrainTarget(kind, *resource, parsed_domain);
}

inline drain::DrainTarget target_of(const std::string& id, const std::string& domain = "") {
  return make_target(drain::TargetKind::Resource, id, domain);
}

inline drain::HolderId holder_of(const std::string& id) {
  const auto parsed = drain::HolderId::parse(id);
  DRAIN_CHECK_MSG(parsed.has_value(), "test holder id must be a valid slug");
  return *parsed;
}

/// A test double for adjacent runtimes. It records what Drain Fabric asked for
/// and, when configured to answer, reports obligations back through the public
/// engine API.
class ScriptedSink final : public drain::EvacuationSink {
 public:
  enum class Mode {
    /// Record requests and do nothing: the drain must block or wait.
    RecordOnly,
    /// Answer every evacuation request by releasing the obligation.
    AutoRelease,
  };

  explicit ScriptedSink(drain::DrainEngine& engine) : engine_(&engine) {}

  Mode mode{Mode::AutoRelease};
  bool omit_evidence{false};
  bool use_stale_evidence{false};
  bool fail_requests{false};
  bool answer_with_wrong_generation{false};

  std::vector<drain::EvacuationRequest> evacuations{};
  std::vector<drain::RestorationRequest> restorations{};
  std::vector<drain::AdmissionReopenRequest> reopens{};
  /// Incremented when a hook is invoked while the engine mutex is held. This
  /// must stay zero: the engine never calls out under its lock.
  std::size_t calls_under_lock{0};

  drain::Status on_evacuation_request(const drain::EvacuationRequest& request) override {
    if (engine_->lock_held_by_this_thread()) {
      ++calls_under_lock;
    }
    evacuations.push_back(request);
    if (fail_requests) {
      return drain::fail(drain::ErrorCode::TransportError, "scripted sink failure");
    }
    if (mode == Mode::RecordOnly) {
      return drain::ok_status();
    }
    auto obligation = engine_->obligation(request.obligation);
    if (!obligation.has_value()) {
      return drain::ok_status();
    }
    const drain::TimePoint now = engine_->now();
    drain::EvidenceSeq evidence_seq{};
    if (!omit_evidence) {
      const drain::EvidenceKey key{drain::EvidenceKind::ObligationReleased, request.drain,
                                   request.obligation, drain::ResourceId{}};
      const std::uint64_t sequence = next_sequence(key);
      drain::Evidence evidence;
      evidence.seq = drain::EvidenceSeq{sequence};
      evidence.key = key;
      evidence.generation = request.generation;
      evidence.epoch = engine_->epoch();
      evidence.producer = engine_->incarnation();
      evidence.observed_at = use_stale_evidence ? drain::TimePoint{0} : now;
      evidence.healthy = true;
      evidence.detail = "scripted adjacent runtime released the obligation";
      auto recorded = engine_->record_evidence(evidence, now);
      if (!recorded.ok()) {
        return recorded.error();
      }
      evidence_seq = recorded->seq;
    }
    drain::ObligationReport report;
    report.obligation = request.obligation;
    report.expected_revision = obligation->revision();
    report.next = drain::ObligationState::Released;
    report.evidence = evidence_seq;
    report.drain = request.drain;
    report.generation = answer_with_wrong_generation ? drain::Generation{request.generation.value() + 1000}
                                                      : request.generation;
    report.note = "scripted release";
    return engine_->report_obligation(report, now);
  }

  drain::Status on_restoration_request(const drain::RestorationRequest& request) override {
    if (engine_->lock_held_by_this_thread()) {
      ++calls_under_lock;
    }
    restorations.push_back(request);
    return drain::ok_status();
  }

  drain::Status on_admission_reopen(const drain::AdmissionReopenRequest& request) override {
    if (engine_->lock_held_by_this_thread()) {
      ++calls_under_lock;
    }
    reopens.push_back(request);
    return drain::ok_status();
  }

 private:
  std::uint64_t next_sequence(const drain::EvidenceKey& key) {
    for (auto& entry : sequences_) {
      if (entry.first == key) {
        return ++entry.second;
      }
    }
    sequences_.emplace_back(key, 1);
    return 1;
  }

  drain::DrainEngine* engine_{nullptr};
  std::vector<std::pair<drain::EvidenceKey, std::uint64_t>> sequences_{};
};

/// Builds a small but realistic topology:
///   n resources per domain, fully connected inside a domain, with one
///   diversity group over declared paths, one capacity pool, and one protected
///   route between the first two resources.
struct TopologyShape {
  std::size_t domains{2};
  std::size_t resources_per_domain{3};
  std::uint64_t capacity{100};
  std::size_t paths_per_domain{3};
  std::size_t required_paths{2};
};

inline std::vector<drain::DrainTarget> build_topology(drain::DrainEngine& engine,
                                                      const TopologyShape& shape,
                                                      drain::TimePoint now) {
  DRAIN_CHECK_MSG(shape.resources_per_domain > 0, "a domain needs at least one resource");
  DRAIN_CHECK_MSG(shape.paths_per_domain > 0, "a domain needs at least one declared path");
  DRAIN_CHECK_MSG(shape.required_paths <= shape.paths_per_domain,
                  "the diversity commitment cannot exceed the number of declared paths");
  std::vector<drain::DrainTarget> targets;
  for (std::size_t domain = 0; domain < shape.domains; ++domain) {
    const std::string domain_name = "d" + std::to_string(domain);
    std::vector<drain::ResourceId> members;
    for (std::size_t index = 0; index < shape.resources_per_domain; ++index) {
      const std::string id = "r" + std::to_string(domain) + "_" + std::to_string(index);
      const auto target = target_of(id, domain_name);
      DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(shape.capacity)),
                                         now));
      members.push_back(target.id());
      targets.push_back(target);
    }
    for (std::size_t left = 0; left < members.size(); ++left) {
      for (std::size_t right = left + 1; right < members.size(); ++right) {
        DRAIN_CHECK_OK(engine.add_edge(members[left], members[right], now));
      }
    }
    drain::DiversityGroup group;
    group.id = *drain::GroupId::parse("dg" + std::to_string(domain));
    group.domain = *drain::DomainId::parse(domain_name);
    group.required_available = drain::MemberCount::from(shape.required_paths);
    for (std::size_t index = 0; index < shape.paths_per_domain; ++index) {
      drain::FabricPath path;
      const std::string path_name = "p" + std::to_string(domain) + "_" + std::to_string(index);
      path.id = *drain::ResourceId::parse(path_name);
      path.domain = group.domain;
      // Each declared path is a single-member path so that removing one
      // resource costs exactly one path of the group.
      path.hops.push_back(members[index % members.size()]);
      DRAIN_CHECK_OK(engine.add_path(path, now));
      group.paths.push_back(path.id);
    }
    DRAIN_CHECK_OK(engine.add_diversity_group(group, now));

    drain::CapacityPool pool;
    pool.id = *drain::GroupId::parse("cp" + std::to_string(domain));
    pool.domain = group.domain;
    pool.required = drain::CapacityUnits::from(shape.capacity / 2);
    pool.members = members;
    DRAIN_CHECK_OK(engine.add_capacity_pool(pool, now));

    // Connectivity commitments are deliberately not part of the shared shape:
    // a route that spans the whole domain would be severed by draining any
    // endpoint, which would make every correlated request fail for a reason
    // unrelated to the constraint under test. Cases that exercise route loss
    // declare their own routes.

  }
  return targets;
}

}  // namespace drain_test
