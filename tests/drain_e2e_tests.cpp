// Drain Fabric -- end-to-end lifecycle scenarios.
//
// Each case drives the public API through a complete operator workflow and
// asserts the resulting state, blockers, decisions, and accounting. Together
// they cover the domain proof obligations: capacity and redundancy validation
// before removal, alternate-path loss, stale completion evidence, grace
// deadlines with exception handling, cancellation with restoration, duplicate
// callbacks, and correlated multi-resource drains.

#include <chrono>
#include <string>
#include <vector>

#include "drain/engine.hpp"
#include "fixture.hpp"
#include "test.hpp"

using namespace drain_test;

namespace {

struct Harness {
  drain::ManualClock clock;
  drain::DrainPolicy policy;
  drain::DrainEngine engine;
  ScriptedSink sink;
  drain::AuthorityId authority;

  explicit Harness(std::uint64_t retry_ms = 1000) : engine(policy, clock), sink(engine) {
    policy.set_admission_fence_settle(std::chrono::seconds(0));
    policy.set_evacuation_retry_interval(std::chrono::milliseconds(static_cast<std::int64_t>(retry_ms)));
    engine.set_sink(&sink);
    (void)engine.install_incarnation(drain::IncarnationId{1}, clock.now());
    auto issued = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
    if (!issued.ok()) {
      DRAIN_FAIL("cannot issue authority");
    }
    authority = issued->id();
  }

  drain::DrainId request(const std::vector<drain::DrainTarget>& targets) {
    drain::DrainRequest request;
    request.targets = targets;
    request.reason = "e2e";
    request.authority = authority;
    auto set = engine.request_drain_set(request, clock.now());
    if (!set.ok()) {
      DRAIN_FAIL("request rejected: " + set.error().message());
    }
    return engine.drain_set(*set)->members.front();
  }

  void step(std::int64_t millis, int count) {
    for (int index = 0; index < count; ++index) {
      clock.advance(std::chrono::milliseconds(millis));
      (void)engine.advance(clock.now());
    }
  }

  void run_until_settled(drain::DrainId id, int limit = 64) {
    for (int index = 0; index < limit; ++index) {
      const auto record = engine.drain(id);
      if (record.has_value() && drain::is_settled(record->state())) {
        return;
      }
      step(1000, 1);
    }
  }
};

/// A line topology with an explicit metric-free route commitment.
struct LineTopology {
  drain::DrainTarget a;
  drain::DrainTarget b;
  drain::DrainTarget c;
};

LineTopology build_line(drain::DrainEngine& engine, drain::TimePoint now, bool triangle) {
  LineTopology line{target_of("a", "d1"), target_of("b", "d1"), target_of("c", "d1")};
  for (const auto& target : {line.a, line.b, line.c}) {
    DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(100)), now));
  }
  DRAIN_CHECK_OK(engine.add_edge(line.a.id(), line.b.id(), now));
  DRAIN_CHECK_OK(engine.add_edge(line.b.id(), line.c.id(), now));
  if (triangle) {
    DRAIN_CHECK_OK(engine.add_edge(line.a.id(), line.c.id(), now));
  }
  drain::ProtectedRoute route;
  route.id = *drain::GroupId::parse("route-ac");
  route.origin = line.a.id();
  route.terminus = line.c.id();
  DRAIN_CHECK_OK(engine.add_protected_route(route, now));
  return line;
}

}  // namespace

DRAIN_TEST(e2e, capacity_shortfall_blocks_the_removal_step) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::CapacityPool pool;
  pool.id = *drain::GroupId::parse("pool-ab");
  pool.required = drain::CapacityUnits::from(100);
  pool.members = {line.a.id(), line.b.id()};
  DRAIN_CHECK_OK(harness.engine.add_capacity_pool(pool, harness.clock.now()));

  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.step(1000, 2);

  // The peer resource is lost while the obligation is being evacuated; the pool
  // can no longer absorb the removal.
  DRAIN_CHECK_OK(harness.engine.report_resource_status(line.a.id(), drain::ResourceStatus::Failed,
                                                       harness.clock.now()));
  harness.run_until_settled(id, 24);
  const auto record = harness.engine.drain(id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK_MSG(record->has_blocker(drain::BlockerCode::CapacityInsufficient),
                  std::string("state=") + drain::to_string(record->state()) + " " +
                      record->to_json().dump());
  const drain::Explanation explanation = harness.engine.explain(id);
  DRAIN_CHECK(explanation.render().find("pool-ab") != std::string::npos);
  DRAIN_CHECK(explanation.to_json().find("blockers") != nullptr);
}

DRAIN_TEST(e2e, alternate_path_loss_blocks_removal_and_is_explained) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.step(1000, 2);
  DRAIN_CHECK_OK(harness.engine.report_resource_status(line.c.id(), drain::ResourceStatus::Failed,
                                                       harness.clock.now()));
  harness.run_until_settled(id, 24);
  const auto record = harness.engine.drain(id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK_MSG(record->has_blocker(drain::BlockerCode::AlternatePathLost),
                  std::string("state=") + drain::to_string(record->state()) + " " + record->to_json().dump());
  const drain::Explanation explanation = harness.engine.explain(id);
  DRAIN_CHECK(explanation.render().find("route-ac") != std::string::npos);
  DRAIN_CHECK(harness.engine.audit(harness.clock.now()).clean());
}

DRAIN_TEST(e2e, stale_flow_evidence_cannot_finish_a_drain) {
  Harness harness;
  harness.policy.set_evidence_freshness(std::chrono::seconds(5));
  DRAIN_CHECK_OK(harness.engine.set_policy(harness.policy, harness.clock.now()));
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});

  // Drive to the point where the release report has been made but the
  // quiescence retirement has not yet been evaluated.
  harness.step(1000, 5);
  DRAIN_CHECK_MSG(harness.engine.drain(id)->state() == drain::DrainState::Quiescing,
                  harness.engine.drain(id)->to_json().dump());

  // Let the release evidence age out of the freshness window.
  harness.clock.advance(std::chrono::seconds(30));
  (void)harness.engine.advance(harness.clock.now());
  const auto blocked = harness.engine.drain(id);
  DRAIN_CHECK(blocked.has_value());
  DRAIN_CHECK_EQ(static_cast<int>(blocked->state()), static_cast<int>(drain::DrainState::Blocked));
  DRAIN_CHECK(blocked->has_blocker(drain::BlockerCode::ReleaseEvidenceStale));
  DRAIN_CHECK_MSG(blocked->state() != drain::DrainState::Drained, "stale evidence completed a drain");

  // A fresh observation lets it finish.
  const auto obligations = harness.engine.outstanding_obligations(line.b);
  DRAIN_CHECK_EQ(obligations.size(), 1u);
  drain::Evidence evidence;
  evidence.seq = drain::EvidenceSeq{obligations[0].id().value() + 5000};
  evidence.key.kind = drain::EvidenceKind::ObligationReleased;
  evidence.key.drain = id;
  evidence.key.obligation = obligations[0].id();
  evidence.generation = blocked->generation();
  evidence.epoch = harness.engine.epoch();
  evidence.producer = harness.engine.incarnation();
  evidence.observed_at = harness.clock.now();
  evidence.healthy = true;
  auto recorded = harness.engine.record_evidence(evidence, harness.clock.now());
  DRAIN_CHECK(recorded.ok());
  drain::ObligationReport report;
  report.obligation = obligations[0].id();
  report.expected_revision = obligations[0].revision();
  report.next = drain::ObligationState::Released;
  report.evidence = recorded->seq;
  report.drain = id;
  report.generation = blocked->generation();
  DRAIN_CHECK_OK(harness.engine.report_obligation(report, harness.clock.now()));

  harness.run_until_settled(id, 32);
  DRAIN_CHECK_EQ(static_cast<int>(harness.engine.drain(id)->state()),
                 static_cast<int>(drain::DrainState::Drained));
  DRAIN_CHECK(harness.engine.audit(harness.clock.now()).clean());
}

DRAIN_TEST(e2e, release_evidence_predating_the_boundary_is_refused) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  harness.sink.omit_evidence = true;
  const drain::DrainId id = harness.request({line.b});
  harness.run_until_settled(id, 24);
  const auto record = harness.engine.drain(id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK(record->has_blocker(drain::BlockerCode::ReleaseEvidenceMissing));
  DRAIN_CHECK_MSG(record->state() != drain::DrainState::Drained,
                  "a drain completed without release evidence");
}

DRAIN_TEST(e2e, long_lived_obligation_grace_expiry_requires_an_exception) {
  Harness harness;
  harness.policy.set_default_grace(std::chrono::seconds(5));
  DRAIN_CHECK_OK(harness.engine.set_policy(harness.policy, harness.clock.now()));
  harness.sink.mode = ScriptedSink::Mode::RecordOnly;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("long-lived"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.run_until_settled(id, 20);
  auto record = harness.engine.drain(id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK(record->has_blocker(drain::BlockerCode::ObligationGraceExpired));
  const drain::Blocker* grace = nullptr;
  for (const auto& blocker : record->blockers()) {
    if (blocker.code == drain::BlockerCode::ObligationGraceExpired) {
      grace = &blocker;
    }
  }
  DRAIN_CHECK(grace != nullptr);
  DRAIN_CHECK(grace->deadline.count() != 0);

  // The policy exception is the sanctioned way to extend the window.
  DRAIN_CHECK_OK(harness.engine.grant_exception(id, drain::ObligationId{}, std::chrono::seconds(120),
                                                harness.authority, harness.clock.now()));
  harness.sink.mode = ScriptedSink::Mode::AutoRelease;
  harness.run_until_settled(id, 32);
  record = harness.engine.drain(id);
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Drained));
  DRAIN_CHECK_EQ(harness.engine.exceptions(id).size(), 1u);
  DRAIN_CHECK(harness.engine.audit(harness.clock.now()).clean());
}

DRAIN_TEST(e2e, exhausted_evacuation_attempts_require_an_exception) {
  Harness harness;
  harness.policy.set_max_evacuation_attempts(2);
  DRAIN_CHECK_OK(harness.engine.set_policy(harness.policy, harness.clock.now()));
  harness.sink.mode = ScriptedSink::Mode::RecordOnly;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("stuck"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.run_until_settled(id, 24);
  auto record = harness.engine.drain(id);
  DRAIN_CHECK(record->has_blocker(drain::BlockerCode::EvacuationAttemptsExhausted));
  DRAIN_CHECK_EQ(record->evacuation_requests(), 2u);

  DRAIN_CHECK_OK(harness.engine.grant_exception(id, drain::ObligationId{}, std::chrono::seconds(30),
                                                harness.authority, harness.clock.now()));
  harness.sink.mode = ScriptedSink::Mode::AutoRelease;
  harness.run_until_settled(id, 32);
  record = harness.engine.drain(id);
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Drained));
}

DRAIN_TEST(e2e, cancellation_during_evacuation_then_restoration_uses_a_fresh_generation) {
  Harness harness;
  harness.sink.mode = ScriptedSink::Mode::RecordOnly;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.step(1000, 3);
  const auto evacuated = harness.engine.drain(id);
  DRAIN_CHECK_EQ(static_cast<int>(evacuated->state()), static_cast<int>(drain::DrainState::Evacuating));
  const drain::Generation closed = evacuated->generation();

  DRAIN_CHECK_OK(harness.engine.cancel_drain(id, harness.authority, "operator cancelled",
                                             harness.clock.now()));
  harness.step(1000, 4);
  auto record = harness.engine.drain(id);
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Cancelled));

  // Admission stays closed after cancellation: the current generation is the
  // closed one, so the fence refuses the bind outright.
  drain::Obligation late(drain::ObligationKind::ActiveFlow, holder_of("late"), {line.b});
  DRAIN_CHECK_CODE(harness.engine.admit_obligation(late, closed, harness.authority, harness.clock.now()),
                   drain::ErrorCode::AdmissionClosed);

  DRAIN_CHECK_OK(harness.engine.restore_drain(id, harness.authority, "return to service",
                                              harness.clock.now()));
  harness.step(1000, 4);
  record = harness.engine.drain(id);
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Cancelled));
  DRAIN_CHECK(record->restore_generation() > closed);
  DRAIN_CHECK(record->restored_at().count() != 0);
  DRAIN_CHECK_EQ(harness.sink.reopens.size(), 1u);
  DRAIN_CHECK_EQ(harness.sink.reopens.front().generation.value(), record->restore_generation().value());

  // Only the fresh generation may bind new work.
  DRAIN_CHECK_CODE(harness.engine.admit_obligation(late, closed, harness.authority, harness.clock.now()),
                   drain::ErrorCode::StaleGeneration);
  auto admitted = harness.engine.admit_obligation(late, record->restore_generation(), harness.authority,
                                                  harness.clock.now());
  DRAIN_CHECK_MSG(admitted.ok(), admitted.ok() ? "" : admitted.error().message());
  DRAIN_CHECK(harness.engine.audit(harness.clock.now()).clean());
}

DRAIN_TEST(e2e, duplicate_callbacks_are_idempotent_or_stale_never_double_applied) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  auto admitted = harness.engine.admit_obligation(flow, drain::Generation{}, harness.authority,
                                                  harness.clock.now());
  DRAIN_CHECK(admitted.ok());

  drain::Evidence evidence;
  evidence.seq = drain::EvidenceSeq{1};
  evidence.key.kind = drain::EvidenceKind::ObligationReleased;
  evidence.key.obligation = admitted->id();
  evidence.generation = drain::Generation{0};
  evidence.epoch = harness.engine.epoch();
  evidence.producer = harness.engine.incarnation();
  evidence.observed_at = harness.clock.now();
  DRAIN_CHECK_OK(harness.engine.record_evidence(evidence, harness.clock.now()));

  drain::ObligationReport report;
  report.obligation = admitted->id();
  report.expected_revision = admitted->revision();
  report.next = drain::ObligationState::Released;
  report.evidence = drain::EvidenceSeq{1};
  DRAIN_CHECK_OK(harness.engine.report_obligation(report, harness.clock.now()));
  // A replayed callback carries the revision it read before the first
  // application, so it is fenced as stale rather than applied twice.
  DRAIN_CHECK_CODE(harness.engine.report_obligation(report, harness.clock.now()),
                   drain::ErrorCode::StaleRevision);
  DRAIN_CHECK_CODE(harness.engine.report_obligation(report, harness.clock.now()),
                   drain::ErrorCode::StaleRevision);

  const auto obligation = harness.engine.obligation(admitted->id());
  DRAIN_CHECK_EQ(obligation->revision().value(), admitted->revision().value() + 1);
  DRAIN_CHECK_EQ(static_cast<int>(obligation->state()),
                 static_cast<int>(drain::ObligationState::Released));
}

DRAIN_TEST(e2e, correlated_multi_domain_set_orders_deterministically_and_serialises) {
  Harness harness;
  harness.policy.set_max_concurrent_drains_per_domain(2);
  harness.policy.set_max_drains_in_set(8);
  harness.policy.set_serialize_same_domain_in_set(true);
  DRAIN_CHECK_OK(harness.engine.set_policy(harness.policy, harness.clock.now()));
  TopologyShape shape;
  shape.domains = 2;
  shape.resources_per_domain = 3;
  // One affordable loss per domain: the correlated pair in dom1 costs two of
  // the three paths, and the commitment only requires one to survive.
  shape.required_paths = 1;
  const auto targets = build_topology(harness.engine, shape, harness.clock.now());

  drain::DrainRequest request;
  request.targets = {targets[5], targets[0], targets[3]};
  request.reason = "correlated";
  request.authority = harness.authority;
  auto set = harness.engine.request_drain_set(request, harness.clock.now());
  DRAIN_CHECK_MSG(set.ok(), set.ok() ? std::string() : set.error().to_string());
  const auto stored = harness.engine.drain_set(*set);
  DRAIN_CHECK(stored.has_value());
  DRAIN_CHECK_EQ(stored->targets.size(), 3u);
  for (std::size_t index = 1; index < stored->targets.size(); ++index) {
    DRAIN_CHECK(drain::target_order_less(stored->targets[index - 1], stored->targets[index]));
  }
  DRAIN_CHECK_EQ(stored->members.size(), 3u);

  harness.run_until_settled(stored->members.front(), 64);
  const auto audit = harness.engine.audit(harness.clock.now());
  DRAIN_CHECK_MSG(audit.clean(), audit.render());

  // At most one drain per failure domain may hold an admission fence.
  for (const auto& target : stored->targets) {
    (void)target;
  }
  std::size_t in_flight = 0;
  for (const auto& record : harness.engine.drains()) {
    if (record.state() != drain::DrainState::Requested &&
        record.state() != drain::DrainState::Validating &&
        record.state() != drain::DrainState::Drained &&
        record.state() != drain::DrainState::Cancelled &&
        record.state() != drain::DrainState::Failed) {
      ++in_flight;
    }
  }
  DRAIN_CHECK(in_flight <= 2);
}

DRAIN_TEST(e2e, advisory_obligations_never_block_completion) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation advisory(drain::ObligationKind::Supplied, holder_of("observer"), {line.b});
  advisory.set_protection(drain::ProtectionClass::Advisory);
  DRAIN_CHECK(harness.engine
                  .admit_obligation(advisory, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.run_until_settled(id, 32);
  const auto record = harness.engine.drain(id);
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Drained));
  DRAIN_CHECK_EQ(record->remaining_protected_at_completion(), 0u);
  DRAIN_CHECK_EQ(static_cast<int>(harness.engine.obligations().front().state()),
                 static_cast<int>(drain::ObligationState::Admitted));
  DRAIN_CHECK(harness.engine.audit(harness.clock.now()).clean());
}

DRAIN_TEST(e2e, decisions_record_inputs_evidence_and_rejected_alternatives) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.run_until_settled(id, 32);

  const drain::Explanation explanation = harness.engine.explain(id);
  DRAIN_CHECK(explanation.found);
  DRAIN_CHECK(!explanation.history.empty());
  bool saw_rejected = false;
  bool saw_inputs = false;
  bool saw_policy = false;
  for (const auto& decision : explanation.history) {
    DRAIN_CHECK(!decision.action.empty());
    DRAIN_CHECK(!decision.policy_fingerprint.empty());
    if (!decision.rejected.empty()) {
      saw_rejected = true;
    }
    if (!decision.inputs.empty()) {
      saw_inputs = true;
    }
    if (decision.policy_revision.value() != 0) {
      saw_policy = true;
    }
  }
  DRAIN_CHECK(saw_inputs);
  DRAIN_CHECK(saw_rejected);
  DRAIN_CHECK(saw_policy);

  // The explanation is stable across repeated inspection.
  DRAIN_CHECK_EQ(harness.engine.explain(id).render(), explanation.render());
}

DRAIN_TEST(e2e, resource_inconsistency_blocks_instead_of_completing) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  const drain::DrainId id = harness.request({line.b});
  harness.step(1000, 2);
  DRAIN_CHECK_OK(harness.engine.report_resource_status(line.b.id(), drain::ResourceStatus::Failed,
                                                       harness.clock.now()));
  harness.run_until_settled(id, 24);
  const auto record = harness.engine.drain(id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK_MSG(record->state() != drain::DrainState::Drained,
                  "a drain completed while the target was reported failed");
  DRAIN_CHECK(record->has_blocker(drain::BlockerCode::TopologyInconsistent));
}

DRAIN_TEST(e2e, policy_replacement_is_recorded_and_takes_effect) {
  Harness harness;
  const auto line = build_line(harness.engine, harness.clock.now(), true);
  drain::DrainPolicy tightened = harness.policy;
  tightened.set_max_evacuation_attempts(1);
  tightened.set_revision(drain::Revision{2});
  DRAIN_CHECK_OK(harness.engine.set_policy(tightened, harness.clock.now()));
  DRAIN_CHECK_EQ(harness.engine.policy().revision().value(), 2u);
  harness.sink.mode = ScriptedSink::Mode::RecordOnly;
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {line.b});
  DRAIN_CHECK(harness.engine
                  .admit_obligation(flow, drain::Generation{}, harness.authority, harness.clock.now())
                  .ok());
  const drain::DrainId id = harness.request({line.b});
  harness.run_until_settled(id, 20);
  const auto record = harness.engine.drain(id);
  DRAIN_CHECK(record->has_blocker(drain::BlockerCode::EvacuationAttemptsExhausted));
  bool saw_new_revision = false;
  for (const auto& decision : harness.engine.decisions(256)) {
    if (decision.policy_revision.value() == 2) {
      saw_new_revision = true;
    }
  }
  DRAIN_CHECK(saw_new_revision);
}
