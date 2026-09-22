// Drain Fabric -- persistence and restart tests.
//
// These cases prove the recovery contract: durable state is versioned and
// integrity checked, a restart always advances the incarnation, recovered
// evidence is quarantined rather than promoted, transient drain states are
// conservatively blocked and revalidated, and authority never survives a
// restart.

#include <chrono>
#include <string>
#include <vector>

#include "drain/engine.hpp"
#include "drain/persistence.hpp"
#include "drain/runtime.hpp"
#include "fixture.hpp"
#include "process.hpp"
#include "test.hpp"

using namespace drain_test;

namespace {

drain::RuntimeOptions runtime_options_for(const std::string& path) {
  drain::RuntimeOptions options;
  options.state_path = path;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  policy.set_evidence_freshness(std::chrono::seconds(600));
  policy.set_default_grace(std::chrono::seconds(600));
  options.policy = policy;
  return options;
}

void seed_world(drain::DrainEngine& engine, drain::ManualClock& clock,
                std::vector<drain::DrainTarget>& targets, drain::AuthorityId& authority) {
  const drain::TimePoint start = clock.now();
  auto issued = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  DRAIN_CHECK(issued.ok());
  authority = issued->id();
  TopologyShape shape;
  shape.domains = 1;
  shape.resources_per_domain = 2;
  shape.paths_per_domain = 2;
  shape.required_paths = 1;
  targets = build_topology(engine, shape, start);
}

drain::Evidence release_evidence(drain::DrainEngine& engine, drain::DrainId drain_id,
                                 drain::ObligationId obligation_id, drain::Generation generation,
                                 std::uint64_t sequence) {
  drain::Evidence evidence;
  evidence.seq = drain::EvidenceSeq{sequence};
  evidence.key.kind = drain::EvidenceKind::ObligationReleased;
  evidence.key.drain = drain_id;
  evidence.key.obligation = obligation_id;
  evidence.generation = generation;
  evidence.epoch = engine.epoch();
  evidence.producer = engine.incarnation();
  evidence.observed_at = engine.now();
  evidence.healthy = true;
  evidence.detail = "restart test release";
  return evidence;
}

}  // namespace

DRAIN_TEST(restart, runtime_advances_the_incarnation_on_every_start) {
  const std::string directory = make_temp_directory("restart-incarnation");
  const std::string path = directory + "/state.drainlog";
  drain::SystemClock clock;
  std::vector<drain::IncarnationId> incarnations;
  for (int boot = 0; boot < 3; ++boot) {
    drain::Runtime runtime(runtime_options_for(path), clock);
    DRAIN_CHECK_OK(runtime.start(clock.now()));
    incarnations.push_back(runtime.incarnation());
    DRAIN_CHECK_OK(runtime.save(clock.now()));
    DRAIN_CHECK_OK(runtime.stop(clock.now()));
  }
  DRAIN_CHECK(incarnations[0] < incarnations[1]);
  DRAIN_CHECK(incarnations[1] < incarnations[2]);
  DRAIN_CHECK_OK(drain::Persistence(path).reset());
}

DRAIN_TEST(restart, recovered_evidence_is_quarantined_and_cannot_be_attested) {
  const std::string directory = make_temp_directory("restart-evidence");
  const std::string path = directory + "/state.drainlog";
  drain::ManualClock clock;
  std::vector<drain::DrainTarget> targets;
  drain::AuthorityId authority{};
  drain::DrainId drain_id{};

  {
    drain::Runtime runtime(runtime_options_for(path), clock);
    drain::DrainEngine& engine = runtime.engine();
    DRAIN_CHECK_OK(runtime.start(clock.now()));
    seed_world(engine, clock, targets, authority);

    drain::Evidence evidence;
    evidence.seq = drain::EvidenceSeq{1};
    evidence.key.kind = drain::EvidenceKind::FlowQuiesced;
    evidence.key.resource = targets[0].id();
    evidence.generation = drain::Generation{1};
    evidence.epoch = engine.epoch();
    evidence.producer = engine.incarnation();
    evidence.observed_at = clock.now();
    DRAIN_CHECK_OK(engine.record_evidence(evidence, clock.now()));

    drain::DrainRequest request;
    request.targets = {targets[0]};
    request.authority = authority;
    auto set = engine.request_drain_set(request, clock.now());
    DRAIN_CHECK(set.ok());
    drain_id = engine.drain_set(*set)->members.front();
    (void)engine.advance(clock.now());
    (void)engine.advance(clock.now());
    DRAIN_CHECK_EQ(static_cast<int>(engine.drain(drain_id)->state()),
                   static_cast<int>(drain::DrainState::AdmissionClosed));
    DRAIN_CHECK_OK(runtime.save(clock.now()));
  }

  {
    drain::Runtime runtime(runtime_options_for(path), clock);
    DRAIN_CHECK_OK(runtime.start(clock.now() + std::chrono::seconds(1)));
    const auto record = runtime.engine().drain(drain_id);
    DRAIN_CHECK(record.has_value());
    DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Blocked));
    DRAIN_CHECK(record->has_blocker(drain::BlockerCode::RestartRecoveryPending));

    const auto evidence_records = runtime.engine().evidence_records();
    DRAIN_CHECK_EQ(evidence_records.size(), 1u);
    DRAIN_CHECK(!evidence_records.front().attested);
    // Recovered evidence is fenced by both the superseded epoch and the
    // previous incarnation; either rejection is a stale-fence rejection.
    drain::EvidenceKey quiesced_key;
    quiesced_key.kind = drain::EvidenceKind::FlowQuiesced;
    quiesced_key.resource = targets[0].id();
    const drain::Status attest =
        runtime.engine().attest_evidence(quiesced_key, drain::EvidenceSeq{1}, clock.now());
    DRAIN_CHECK(!attest.ok());
    DRAIN_CHECK(drain::is_stale_fence(attest.code()));
    DRAIN_CHECK(runtime.engine().audit(clock.now()).clean());
  }
}

DRAIN_TEST(restart, a_completed_drain_survives_and_stays_auditable) {
  const std::string directory = make_temp_directory("restart-completed");
  const std::string path = directory + "/state.drainlog";
  drain::ManualClock clock;
  std::vector<drain::DrainTarget> targets;
  drain::AuthorityId authority{};
  drain::DrainId drain_id{};
  std::uint64_t digest = 0;

  {
    drain::Runtime runtime(runtime_options_for(path), clock);
    drain::DrainEngine& engine = runtime.engine();
    DRAIN_CHECK_OK(runtime.start(clock.now()));
    seed_world(engine, clock, targets, authority);
    drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("h"), {targets[0]});
    DRAIN_CHECK(engine.admit_obligation(flow, drain::Generation{}, authority, clock.now()).ok());

    drain::DrainRequest request;
    request.targets = {targets[0]};
    request.authority = authority;
    auto set = engine.request_drain_set(request, clock.now());
    DRAIN_CHECK(set.ok());
    drain_id = engine.drain_set(*set)->members.front();
    std::uint64_t sequence = 1000;
    for (int step = 0; step < 48; ++step) {
      clock.advance(std::chrono::seconds(1));
      const auto before = engine.drain(drain_id);
      if (before.has_value() && before->generation().value() != 0) {
        const drain::Generation generation = before->generation();
        for (const auto& obligation : engine.outstanding_obligations(targets[0])) {
          // Evidence is only meaningful once admission has closed, and every
          // observation needs its own sequence.
          auto recorded = engine.record_evidence(
              release_evidence(engine, drain_id, obligation.id(), generation, ++sequence), clock.now());
          if (!recorded.ok()) {
            continue;
          }
          drain::ObligationReport report;
          report.obligation = obligation.id();
          report.expected_revision = obligation.revision();
          report.next = drain::ObligationState::Released;
          report.evidence = recorded->seq;
          report.drain = drain_id;
          report.generation = generation;
          (void)engine.report_obligation(report, clock.now());
        }
      }
      (void)engine.advance(clock.now());
      const auto record = engine.drain(drain_id);
      if (record.has_value() && record->state() == drain::DrainState::Drained) {
        digest = record->completion_digest();
        break;
      }
    }
    DRAIN_CHECK(digest != 0);
    DRAIN_CHECK_OK(runtime.save(clock.now()));
  }

  {
    drain::Runtime runtime(runtime_options_for(path), clock);
    DRAIN_CHECK_OK(runtime.start(clock.now() + std::chrono::seconds(5)));
    const auto record = runtime.engine().drain(drain_id);
    DRAIN_CHECK(record.has_value());
    DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Drained));
    DRAIN_CHECK_EQ(record->completion_digest(), digest);
    DRAIN_CHECK_EQ(record->remaining_protected_at_completion(), 0u);
    const drain::AccountingReport audit = runtime.engine().audit(clock.now());
    DRAIN_CHECK_MSG(audit.clean(), audit.render());
    const drain::Explanation explanation = runtime.engine().explain(drain_id);
    DRAIN_CHECK(explanation.found);
    DRAIN_CHECK(!explanation.history.empty());
  }
}

DRAIN_TEST(restart, transient_evacuating_drain_is_blocked_then_resumes_with_fresh_evidence) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  policy.set_evidence_freshness(std::chrono::seconds(600));

  drain::DrainEngine engine(policy, clock);
  ScriptedSink sink(engine);
  sink.mode = ScriptedSink::Mode::RecordOnly;
  engine.set_sink(&sink);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  std::vector<drain::DrainTarget> targets;
  drain::AuthorityId authority{};
  seed_world(engine, clock, targets, authority);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("h"), {targets[0]});
  DRAIN_CHECK(engine.admit_obligation(flow, drain::Generation{}, authority, clock.now()).ok());

  drain::DrainRequest request;
  request.targets = {targets[0]};
  request.authority = authority;
  auto set = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(set.ok());
  const drain::DrainId drain_id = engine.drain_set(*set)->members.front();
  for (int step = 0; step < 8; ++step) {
    clock.advance(std::chrono::seconds(1));
    (void)engine.advance(clock.now());
  }
  const auto before = engine.drain(drain_id);
  DRAIN_CHECK(before.has_value());
  DRAIN_CHECK_EQ(static_cast<int>(before->state()), static_cast<int>(drain::DrainState::Evacuating));
  const drain::Generation generation = before->generation();
  const drain::JsonValue snapshot = engine.snapshot();

  drain::DrainEngine recovered(policy, clock);
  DRAIN_CHECK_OK(recovered.restore(snapshot, drain::IncarnationId{1}, clock.now()));
  DRAIN_CHECK_OK(recovered.begin_new_epoch(drain::IncarnationId{2}, clock.now(), "restart"));
  auto fresh = recovered.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
  DRAIN_CHECK(fresh.ok());

  const auto blocked = recovered.drain(drain_id);
  DRAIN_CHECK(blocked.has_value());
  DRAIN_CHECK_EQ(static_cast<int>(blocked->state()), static_cast<int>(drain::DrainState::Blocked));
  DRAIN_CHECK(blocked->has_blocker(drain::BlockerCode::RestartRecoveryPending));

  // A report carrying the old generation is still accepted for bookkeeping, but
  // the drain itself must revalidate before it can proceed.
  const auto obligations = recovered.outstanding_obligations(targets[0]);
  DRAIN_CHECK_EQ(obligations.size(), 1u);
  auto recorded = recovered.record_evidence(
      release_evidence(recovered, drain_id, obligations[0].id(), generation, 5000), clock.now());
  DRAIN_CHECK(recorded.ok());
  drain::ObligationReport report;
  report.obligation = obligations[0].id();
  report.expected_revision = obligations[0].revision();
  report.next = drain::ObligationState::Released;
  report.evidence = recorded->seq;
  report.drain = drain_id;
  report.generation = generation;
  DRAIN_CHECK_OK(recovered.report_obligation(report, clock.now()));

  for (int step = 0; step < 24; ++step) {
    clock.advance(std::chrono::seconds(1));
    (void)recovered.advance(clock.now());
    const auto record = recovered.drain(drain_id);
    if (record.has_value() && drain::is_settled(record->state())) {
      break;
    }
  }
  const auto final_record = recovered.drain(drain_id);
  DRAIN_CHECK(final_record.has_value());
  DRAIN_CHECK_EQ(static_cast<int>(final_record->state()), static_cast<int>(drain::DrainState::Drained));
  DRAIN_CHECK_EQ(final_record->generation().value(), generation.value());
  DRAIN_CHECK_MSG(recovered.audit(clock.now()).clean(), recovered.audit(clock.now()).render());
}

DRAIN_TEST(restart, cancelled_drain_stays_fenced_across_a_restart) {
  const std::string directory = make_temp_directory("restart-cancelled");
  const std::string path = directory + "/state.drainlog";
  drain::ManualClock clock;
  std::vector<drain::DrainTarget> targets;
  drain::AuthorityId authority{};
  drain::DrainId drain_id{};

  {
    drain::Runtime runtime(runtime_options_for(path), clock);
    drain::DrainEngine& engine = runtime.engine();
    DRAIN_CHECK_OK(runtime.start(clock.now()));
    seed_world(engine, clock, targets, authority);
    drain::DrainRequest request;
    request.targets = {targets[0]};
    request.authority = authority;
    auto set = engine.request_drain_set(request, clock.now());
    DRAIN_CHECK(set.ok());
    drain_id = engine.drain_set(*set)->members.front();
    (void)engine.advance(clock.now());
    (void)engine.advance(clock.now());
    DRAIN_CHECK_OK(engine.cancel_drain(drain_id, authority, "operator cancelled", clock.now()));
    DRAIN_CHECK_OK(runtime.save(clock.now()));
  }

  {
    drain::Runtime runtime(runtime_options_for(path), clock);
    DRAIN_CHECK_OK(runtime.start(clock.now() + std::chrono::seconds(1)));
    drain::DrainEngine& engine = runtime.engine();
    const auto record = engine.drain(drain_id);
    DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Cancelled));
    DRAIN_CHECK(engine.audit(clock.now()).clean());

    // Admission is still closed: a fresh bind is refused even after recovery.
    drain::Obligation late(drain::ObligationKind::ActiveFlow, holder_of("late"), {targets[0]});
    auto issued = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
    DRAIN_CHECK(issued.ok());
    DRAIN_CHECK_CODE(engine.admit_obligation(late, drain::Generation{}, issued->id(), clock.now()),
                     drain::ErrorCode::StaleGeneration);

    DRAIN_CHECK_OK(engine.restore_drain(drain_id, issued->id(), "return to service", clock.now()));
    for (int step = 0; step < 6; ++step) {
      clock.advance(std::chrono::seconds(1));
      (void)engine.advance(clock.now());
    }
    const auto restored = engine.drain(drain_id);
    DRAIN_CHECK_EQ(static_cast<int>(restored->state()), static_cast<int>(drain::DrainState::Cancelled));
    DRAIN_CHECK(restored->restored_at().count() != 0);
    DRAIN_CHECK(restored->restore_generation() > restored->generation());
    auto readmitted = engine.admit_obligation(late, restored->restore_generation(), issued->id(), clock.now());
    DRAIN_CHECK_MSG(readmitted.ok(), readmitted.ok() ? "" : readmitted.error().message());
    DRAIN_CHECK_MSG(engine.audit(clock.now()).clean(), engine.audit(clock.now()).render());
  }
}

DRAIN_TEST(restart, corrupt_boot_marker_is_detected) {
  const std::string directory = make_temp_directory("restart-marker");
  const std::string path = directory + "/state.drainlog";
  drain::SystemClock clock;
  drain::Runtime first(runtime_options_for(path), clock);
  DRAIN_CHECK_OK(first.start(clock.now()));
  DRAIN_CHECK_OK(first.stop(clock.now()));

  write_text_file(path + ".boot", "DRNFBOOT 1\n12\ndeadbeef\n");
  drain::Runtime second(runtime_options_for(path), clock);
  DRAIN_CHECK_CODE(second.start(clock.now()), drain::ErrorCode::PersistenceCorrupt);

  write_text_file(path + ".boot", "DRNFBOOT 1\nnot-a-number\n00000000\n");
  drain::Runtime third(runtime_options_for(path), clock);
  DRAIN_CHECK_CODE(third.start(clock.now()), drain::ErrorCode::PersistenceCorrupt);

  write_text_file(path + ".boot", "garbage");
  drain::Runtime fourth(runtime_options_for(path), clock);
  DRAIN_CHECK_CODE(fourth.start(clock.now()), drain::ErrorCode::PersistenceCorrupt);
}
