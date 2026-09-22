// Drain Fabric -- randomized property and invariant tests.
//
// Every case builds a randomized obligation graph and topology, then drives the
// engine through a randomized event sequence. After every step the authoritative
// snapshot is audited. The defining invariants are checked directly:
//
//   * accounting closure: the audit reports no violation;
//   * a drained drain has zero remaining protected dependencies;
//   * no obligation binds after admission is authoritatively closed;
//   * a stale generation, epoch, or incarnation never mutates state;
//   * identical inputs produce byte-identical decisions and explanations.

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "drain/engine.hpp"
#include "drain/runtime.hpp"
#include "fixture.hpp"
#include "test.hpp"

using namespace drain_test;

namespace {

/// A randomized world: topology, obligations, drains, and a sink.
struct World {
  drain::ManualClock clock;
  drain::DrainPolicy policy;
  drain::DrainEngine engine;
  ScriptedSink sink;
  std::vector<drain::DrainTarget> targets;
  drain::AuthorityId authority;
  drain::IncarnationId incarnation{1};

  World(std::uint64_t seed, std::size_t resources, std::size_t domains)
      : policy(drain::DrainPolicy::defaults()), engine(policy, clock), sink(engine) {
    (void)seed;
    policy.set_max_concurrent_drains_per_domain(2);
    policy.set_max_drains_in_set(6);
    policy.set_admission_fence_settle(std::chrono::milliseconds(5));
    policy.set_evidence_freshness(std::chrono::seconds(60));
    policy.set_default_grace(std::chrono::seconds(120));
    policy.set_evacuation_retry_interval(std::chrono::seconds(1));
    engine.set_sink(&sink);
    (void)engine.install_incarnation(incarnation, clock.now());
    auto issued = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
    DRAIN_CHECK(issued.ok());
    authority = issued->id();

    for (std::size_t domain = 0; domain < domains; ++domain) {
      const std::string domain_name = "dom" + std::to_string(domain);
      std::vector<drain::ResourceId> members;
      const std::size_t per_domain = std::max<std::size_t>(2, resources / std::max<std::size_t>(1, domains));
      for (std::size_t index = 0; index < per_domain; ++index) {
        const std::string id = "res" + std::to_string(domain) + "x" + std::to_string(index);
        const auto target = target_of(id, domain_name);
        DRAIN_CHECK_OK(
            engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(100)), clock.now()));
        members.push_back(target.id());
        targets.push_back(target);
      }
      for (std::size_t left = 0; left < members.size(); ++left) {
        for (std::size_t right = left + 1; right < members.size(); ++right) {
          DRAIN_CHECK_OK(engine.add_edge(members[left], members[right], clock.now()));
        }
      }
      drain::DiversityGroup group;
      group.id = *drain::GroupId::parse("grp" + std::to_string(domain));
      group.domain = *drain::DomainId::parse(domain_name);
      group.required_available = drain::MemberCount::from(1);
      for (std::size_t index = 0; index < members.size(); ++index) {
        drain::FabricPath path;
        path.id = *drain::ResourceId::parse("pth" + std::to_string(domain) + "x" + std::to_string(index));
        path.domain = group.domain;
        path.hops.push_back(members[index]);
        DRAIN_CHECK_OK(engine.add_path(path, clock.now()));
        group.paths.push_back(path.id);
      }
      DRAIN_CHECK_OK(engine.add_diversity_group(group, clock.now()));
      drain::CapacityPool pool;
      pool.id = *drain::GroupId::parse("pool" + std::to_string(domain));
      pool.domain = group.domain;
      pool.required = drain::CapacityUnits::from(50);
      pool.members = members;
      DRAIN_CHECK_OK(engine.add_capacity_pool(pool, clock.now()));
    }
  }

  drain::TimePoint now() const { return clock.now(); }
};

void audit_or_fail(drain::DrainEngine& engine, drain::TimePoint now, const std::string& context) {
  const drain::AccountingReport audit = engine.audit(now);
  DRAIN_CHECK_MSG(audit.clean(), context + "\n" + audit.render());
}

/// Verifies the defining invariants against the authoritative snapshot.
void check_invariants(drain::DrainEngine& engine, drain::TimePoint now, const std::string& context) {
  audit_or_fail(engine, now, context);
  for (const auto& record : engine.drains()) {
    if (record.state() == drain::DrainState::Drained) {
      const std::size_t outstanding = engine.outstanding_protected_count(record.target());
      DRAIN_CHECK_MSG(outstanding == 0,
                      context + ": drained drain " + std::to_string(record.id().value()) +
                          " still has " + std::to_string(outstanding) + " protected dependency(ies)");
      DRAIN_CHECK_EQ(record.remaining_protected_at_completion(), 0u);
    }
  }
}

std::string run_scenario(std::uint64_t seed, std::size_t steps, bool with_restart) {
  Rng rng(seed);
  World world(seed, 6, 2);
  std::vector<drain::DrainId> drains;
  std::string transcript;

  for (std::size_t step = 0; step < steps; ++step) {
    world.clock.advance(std::chrono::milliseconds(static_cast<std::int64_t>(rng.below(500)) + 1));
    const std::uint64_t action = rng.below(10);
    if (action == 0) {
      // Admit an obligation. Whether the target is fenced decides the outcome,
      // and the outcome must never violate the boundary.
      const auto& owner = world.targets[rng.index(world.targets.size())];
      drain::Obligation draft(drain::ObligationKind::ActiveFlow,
                              holder_of("holder" + std::to_string(rng.below(4))), {owner});
      auto admitted = world.engine.admit_obligation(draft, drain::Generation{}, world.authority, world.now());
      if (!admitted.ok()) {
        const auto code = admitted.error().code();
        DRAIN_CHECK(code == drain::ErrorCode::AdmissionClosed ||
                    code == drain::ErrorCode::StaleGeneration ||
                    code == drain::ErrorCode::BoundsExceeded);
      }
    } else if (action == 1) {
      std::vector<drain::DrainTarget> selection;
      const std::size_t count = 1 + rng.index(2);
      for (std::size_t index = 0; index < count; ++index) {
        selection.push_back(world.targets[rng.index(world.targets.size())]);
      }
      drain::DrainRequest request;
      request.targets = selection;
      request.reason = "randomized";
      request.authority = world.authority;
      request.nonce = drain::RequestNonce{rng.below(1000000) + 1};
      auto set = world.engine.request_drain_set(request, world.now());
      if (set.ok()) {
        const auto stored = world.engine.drain_set(*set);
        DRAIN_CHECK(stored.has_value());
        drains.insert(drains.end(), stored->members.begin(), stored->members.end());
      }
    } else if (action == 2 && !drains.empty()) {
      const drain::DrainId id = drains[rng.index(drains.size())];
      const auto record = world.engine.drain(id);
      if (record.has_value() && !drain::is_settled(record->state())) {
        (void)world.engine.cancel_drain(id, world.authority, "randomized cancel", world.now());
      }
    } else if (action == 3 && !drains.empty()) {
      const drain::DrainId id = drains[rng.index(drains.size())];
      (void)world.engine.restore_drain(id, world.authority, "randomized restore", world.now());
    } else if (action == 4) {
      const auto& victim = world.targets[rng.index(world.targets.size())];
      (void)world.engine.report_resource_status(victim.id(), drain::ResourceStatus::Failed, world.now());
    } else if (action == 5) {
      world.sink.mode = rng.chance(1, 3) ? ScriptedSink::Mode::RecordOnly : ScriptedSink::Mode::AutoRelease;
      world.sink.omit_evidence = rng.chance(1, 5);
    } else {
      for (const auto& record : world.engine.drains()) {
        if (drain::is_settled(record.state())) {
          continue;
        }
        const auto obligations = world.engine.outstanding_obligations(record.target());
        for (const auto& obligation : obligations) {
          drain::ObligationReport report;
          report.obligation = obligation.id();
          report.expected_revision = obligation.revision();
          report.next = drain::ObligationState::Released;
          report.drain = record.id();
          report.generation = record.generation();
          drain::Evidence evidence;
          evidence.key.kind = drain::EvidenceKind::ObligationReleased;
          evidence.key.drain = record.id();
          evidence.key.obligation = obligation.id();
          evidence.seq = drain::EvidenceSeq{100000 + step * 16 + obligation.id().value()};
          evidence.generation = record.generation();
          evidence.epoch = world.engine.epoch();
          evidence.producer = world.engine.incarnation();
          evidence.observed_at = world.now();
          evidence.healthy = true;
          auto recorded = world.engine.record_evidence(evidence, world.now());
          if (recorded.ok()) {
            report.evidence = recorded->seq;
            (void)world.engine.report_obligation(report, world.now());
          }
        }
      }
    }

    (void)world.engine.advance(world.now());
    check_invariants(world.engine, world.now(), "seed " + std::to_string(seed) + " step " +
                                                    std::to_string(step));

    if (with_restart && rng.chance(1, 25)) {
      // Restart: recover into a fresh incarnation and re-check everything.
      const drain::JsonValue snapshot = world.engine.snapshot();
      World successor(seed + 1, 6, 2);
      DRAIN_CHECK_OK(successor.engine.restore(snapshot, world.incarnation, successor.now()));
      DRAIN_CHECK_OK(successor.engine.begin_new_epoch(drain::IncarnationId{world.incarnation.value() + 1},
                                                      successor.now(), "randomized restart"));
      for (int recovery_step = 0; recovery_step < 8; ++recovery_step) {
        successor.clock.advance(std::chrono::seconds(1));
        (void)successor.engine.advance(successor.now());
        check_invariants(successor.engine, successor.now(),
                         "seed " + std::to_string(seed) + " restart recovery");
      }
    }
  }

  for (const auto& decision : world.engine.decisions(64)) {
    transcript.append(decision.render());
  }
  const drain::AccountingReport audit = world.engine.audit(world.now());
  transcript.append(audit.render());
  return transcript;
}

}  // namespace

DRAIN_TEST(property, randomized_lifecycles_close_accounting) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    Rng rng(seed);
    World world(seed, 8, 2);
    for (std::size_t index = 0; index < 6; ++index) {
      const auto& owner = world.targets[rng.index(world.targets.size())];
      drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("h" + std::to_string(index)),
                              {owner});
      (void)world.engine.admit_obligation(draft, drain::Generation{}, world.authority, world.now());
    }
    for (std::size_t round = 0; round < 6; ++round) {
      drain::DrainRequest request;
      request.targets = {world.targets[rng.index(world.targets.size())]};
      request.authority = world.authority;
      request.nonce = drain::RequestNonce{round + 1};
      (void)world.engine.request_drain_set(request, world.now());
      for (int step = 0; step < 40; ++step) {
        world.clock.advance(std::chrono::seconds(1));
        (void)world.engine.advance(world.now());
      }
      check_invariants(world.engine, world.now(), "seed " + std::to_string(seed) + " round " +
                                                      std::to_string(round));
    }
    const drain::AccountingReport audit = world.engine.audit(world.now());
    DRAIN_CHECK_MSG(audit.drains_drained > 0, "no drain completed in seed " + std::to_string(seed));
  }
}

DRAIN_TEST(property, randomized_event_sequences_never_violate_the_boundary) {
  for (std::uint64_t seed = 100; seed <= 115; ++seed) {
    (void)run_scenario(seed, 40, false);
  }
}

DRAIN_TEST(property, restart_during_randomized_sequences_is_conservative) {
  for (std::uint64_t seed = 200; seed <= 209; ++seed) {
    (void)run_scenario(seed, 36, true);
  }
}

DRAIN_TEST(property, identical_inputs_produce_identical_decisions) {
  for (std::uint64_t seed = 300; seed <= 305; ++seed) {
    const std::string first = run_scenario(seed, 32, false);
    const std::string second = run_scenario(seed, 32, false);
    DRAIN_CHECK_MSG(first == second, "decisions diverged for seed " + std::to_string(seed));
    DRAIN_CHECK(!first.empty());
  }
}

DRAIN_TEST(property, admission_never_binds_after_closure) {
  for (std::uint64_t seed = 400; seed <= 415; ++seed) {
    Rng rng(seed);
    World world(seed, 6, 2);
    drain::DrainRequest request;
    request.targets = {world.targets[0]};
    request.authority = world.authority;
    auto set = world.engine.request_drain_set(request, world.now());
    DRAIN_CHECK(set.ok());
    const drain::DrainId id = world.engine.drain_set(*set)->members.front();

    for (int step = 0; step < 4; ++step) {
      world.clock.advance(std::chrono::seconds(1));
      (void)world.engine.advance(world.now());
    }
    const auto record = world.engine.drain(id);
    DRAIN_CHECK(record.has_value());
    const drain::Generation closed = record->generation();
    for (int attempt = 0; attempt < 8; ++attempt) {
      drain::Obligation draft(drain::ObligationKind::ActiveFlow,
                              holder_of("racer" + std::to_string(attempt)), {world.targets[0]});
      auto admitted = world.engine.admit_obligation(draft, closed, world.authority, world.now());
      DRAIN_CHECK(!admitted.ok());
      DRAIN_CHECK(admitted.error().code() == drain::ErrorCode::AdmissionClosed);
      (void)rng.below(3);
    }
    check_invariants(world.engine, world.now(), "closure race seed " + std::to_string(seed));
  }
}

DRAIN_TEST(property, stale_generations_and_epochs_never_mutate_state) {
  World world(900, 6, 2);
  drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("h"), {world.targets[0]});
  auto admitted = world.engine.admit_obligation(draft, drain::Generation{}, world.authority, world.now());
  DRAIN_CHECK(admitted.ok());
  const std::uint64_t digest_before = world.engine.authoritative_digest();

  drain::ObligationReport report;
  report.obligation = admitted->id();
  report.expected_revision = admitted->revision();
  report.next = drain::ObligationState::Released;
  report.generation = drain::Generation{999};
  report.drain = drain::DrainId{1};
  DRAIN_CHECK_CODE(world.engine.report_obligation(report, world.now()), drain::ErrorCode::NotFound);

  drain::DrainRequest request;
  request.targets = {world.targets[0]};
  // A token this incarnation never issued is simply unknown.
  request.authority = drain::AuthorityId{9999};
  DRAIN_CHECK_CODE(world.engine.request_drain_set(request, world.now()),
                   drain::ErrorCode::AuthorityRequired);
  DRAIN_CHECK_EQ(world.engine.authoritative_digest(), digest_before);

  // A token issued by the previous incarnation is a stale fence, not an
  // unknown one, and must never mutate state again.
  auto issued = world.engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), world.now());
  DRAIN_CHECK(issued.ok());
  request.authority = issued->id();
  DRAIN_CHECK_OK(world.engine.begin_new_epoch(drain::IncarnationId{2}, world.now(), "test"));
  DRAIN_CHECK_CODE(world.engine.request_drain_set(request, world.now()),
                   drain::ErrorCode::StaleAuthority);
  DRAIN_CHECK_EQ(world.engine.authoritative_digest(), digest_before);
}

DRAIN_TEST(property, obligation_reappearance_blocks_completion) {
  World world(1000, 6, 2);
  drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("h"), {world.targets[0]});
  auto admitted = world.engine.admit_obligation(draft, drain::Generation{}, world.authority, world.now());
  DRAIN_CHECK(admitted.ok());

  drain::DrainRequest request;
  request.targets = {world.targets[0]};
  request.authority = world.authority;
  auto set = world.engine.request_drain_set(request, world.now());
  DRAIN_CHECK(set.ok());
  const drain::DrainId id = world.engine.drain_set(*set)->members.front();

  for (int step = 0; step < 24; ++step) {
    world.clock.advance(std::chrono::seconds(1));
    (void)world.engine.advance(world.now());
    const auto record = world.engine.drain(id);
    if (record.has_value() && record->state() == drain::DrainState::Drained) {
      break;
    }
  }
  DRAIN_CHECK_EQ(static_cast<int>(world.engine.drain(id)->state()),
                 static_cast<int>(drain::DrainState::Drained));

  // The adjacent runtime re-binds the flow to the drained resource.
  // The query API returns copies: take the value, never a pointer into the
  // temporary vector it came from.
  std::optional<drain::Obligation> obligation;
  for (const auto& candidate : world.engine.obligations()) {
    if (candidate.id() == admitted->id()) {
      obligation = candidate;
    }
  }
  DRAIN_CHECK(obligation.has_value());
  drain::ObligationReport reappear;
  reappear.obligation = admitted->id();
  reappear.expected_revision = obligation->revision();
  reappear.next = drain::ObligationState::Admitted;
  reappear.drain = id;
  reappear.generation = world.engine.drain(id)->generation();
  DRAIN_CHECK_OK(world.engine.report_obligation(reappear, world.now()));
  DRAIN_CHECK_EQ(world.engine.outstanding_protected_count(world.targets[0]), 1u);

  // The audit now reports the inconsistency: a drained target has a live
  // protected dependency. This is exactly the invariant the runtime must never
  // reach on its own, and the audit is what makes it visible.
  const drain::AccountingReport audit = world.engine.audit(world.now());
  DRAIN_CHECK(!audit.clean());
  DRAIN_CHECK(!audit.violations.empty());
}
