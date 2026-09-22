// Drain Fabric benchmark.
//
// Measures completed work, not submission: every number below counts fully
// completed drain lifecycles (admission closed, obligations evacuated and
// retired, removal step validated, verification passed) or completed persistence
// round trips. Reporting enqueue latency would say nothing about the runtime.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "drain/engine.hpp"
#include "drain/persistence.hpp"

namespace {

drain::DrainTarget target(const std::string& id, const std::string& domain) {
  return drain::DrainTarget(drain::TargetKind::Resource, *drain::ResourceId::parse(id),
                            *drain::DomainId::parse(domain));
}

/// Answers every evacuation request immediately: this is the best case for the
/// coordination path and isolates Drain Fabric's own cost.
class InstantSink final : public drain::EvacuationSink {
 public:
  explicit InstantSink(drain::DrainEngine& engine) : engine_(&engine) {}

  drain::Status on_evacuation_request(const drain::EvacuationRequest& request) override {
    auto obligation = engine_->obligation(request.obligation);
    if (!obligation.has_value()) {
      return drain::ok_status();
    }
    drain::Evidence evidence;
    evidence.seq = drain::EvidenceSeq{sequence_++};
    evidence.key.kind = drain::EvidenceKind::ObligationReleased;
    evidence.key.drain = request.drain;
    evidence.key.obligation = request.obligation;
    evidence.generation = request.generation;
    evidence.epoch = engine_->epoch();
    evidence.producer = engine_->incarnation();
    evidence.observed_at = engine_->now();
    evidence.healthy = true;
    auto recorded = engine_->record_evidence(evidence, engine_->now());
    if (!recorded.ok()) {
      return recorded.error();
    }
    drain::ObligationReport report;
    report.obligation = request.obligation;
    report.expected_revision = obligation->revision();
    report.next = drain::ObligationState::Released;
    report.evidence = recorded->seq;
    report.drain = request.drain;
    report.generation = request.generation;
    return engine_->report_obligation(report, engine_->now());
  }
  drain::Status on_restoration_request(const drain::RestorationRequest&) override {
    return drain::ok_status();
  }
  drain::Status on_admission_reopen(const drain::AdmissionReopenRequest&) override {
    return drain::ok_status();
  }

 private:
  drain::DrainEngine* engine_{nullptr};
  std::uint64_t sequence_{1};
};

std::size_t run_batch(std::size_t resource_count, std::size_t obligation_count) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_concurrent_drains_per_domain(1);
  drain::DrainEngine engine(policy, clock);
  InstantSink sink(engine);
  engine.set_sink(&sink);

  const drain::TimePoint start = clock.now();
  (void)engine.install_incarnation(drain::IncarnationId{1}, start);
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);

  std::vector<drain::DrainTarget> targets;
  for (std::size_t index = 0; index < resource_count; ++index) {
    const std::string domain = "d" + std::to_string(index % 4);
    const auto resource = target("res" + std::to_string(index), domain);
    (void)engine.add_resource(drain::ResourceNode(resource, drain::CapacityUnits::from(1000)), start);
    targets.push_back(resource);
  }
  for (std::size_t index = 0; index < obligation_count; ++index) {
    const auto& owner = targets[index % targets.size()];
    drain::Obligation obligation(drain::ObligationKind::ActiveFlow,
                                 *drain::HolderId::parse("holder" + std::to_string(index)), {owner});
    (void)engine.admit_obligation(obligation, drain::Generation{}, authority->id(), clock.now());
  }

  std::size_t completed = 0;
  std::size_t incomplete = 0;
  std::uint64_t first_incomplete = 0;
  for (const auto& resource : targets) {
    drain::DrainRequest request;
    request.targets = {resource};
    request.authority = authority->id();
    request.reason = "benchmark";
    auto set = engine.request_drain_set(request, clock.now());
    if (!set.ok()) {
      continue;
    }
    const auto stored = engine.drain_set(*set);
    const drain::DrainId id = stored->members.front();
    bool settled = false;
    for (int step = 0; step < 512; ++step) {
      clock.advance(std::chrono::milliseconds(10));
      (void)engine.advance(clock.now());
      const auto record = engine.drain(id);
      if (record.has_value() && drain::is_settled(record->state())) {
        settled = true;
        if (record->state() == drain::DrainState::Drained) {
          ++completed;
        }
        break;
      }
    }
    if (!settled) {
      // Reporting why work did not complete is part of measuring completed work
      // honestly.
      const auto record = engine.drain(id);
      std::printf("incomplete drain %llu target=%s state=%s\n",
                  static_cast<unsigned long long>(id.value()), resource.to_string().c_str(),
                  drain::to_string(record->state()));
      std::printf("    record: %s\n", record->to_json().dump().c_str());
      for (const auto& blocker : record->blockers()) {
        std::printf("    blocker: %s\n", blocker.render().c_str());
      }
      if (first_incomplete == 0) {
        first_incomplete = id.value();
        for (const auto& evidence : engine.evidence_records()) {
          if (evidence.key.drain == id) {
            std::printf("    evidence: %s\n", evidence.to_json().dump().c_str());
          }
        }
      }
      ++incomplete;
      if (incomplete >= 3) {
        break;
      }
    }
  }
  return completed;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t resources = argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 64;
  const std::size_t obligations = argc > 2 ? static_cast<std::size_t>(std::stoull(argv[2])) : 256;

  const auto began = std::chrono::steady_clock::now();
  const std::size_t completed = run_batch(resources, obligations);
  const auto finished = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(finished - began).count();
  std::printf("completed drains        : %zu of %zu\n", completed, resources);
  std::printf("obligations per run     : %zu\n", obligations);
  std::printf("elapsed                 : %.6f s\n", seconds);
  std::printf("completed drains/s      : %.1f\n", seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0);

  // Persistence round trip: measures snapshots that were fully written,
  // integrity checked, and parsed back.
  drain::SystemClock clock;
  const std::string path = "drain-benchmark-snapshot.drainlog";
  drain::Persistence persistence(path);
  drain::JsonValue snapshot = drain::JsonValue::object();
  drain::JsonValue entries = drain::JsonValue::array();
  for (std::size_t index = 0; index < 2000; ++index) {
    drain::JsonValue entry = drain::JsonValue::object();
    entry.set("id", drain::JsonValue(static_cast<std::uint64_t>(index)));
    entry.set("state", drain::JsonValue("evacuating"));
    entries.array_ref().push_back(std::move(entry));
  }
  snapshot.set("entries", std::move(entries));

  const auto save_began = std::chrono::steady_clock::now();
  std::size_t round_trips = 0;
  for (int iteration = 0; iteration < 50; ++iteration) {
    drain::Status saved = persistence.save(snapshot, drain::IncarnationId{1}, clock.now());
    if (!saved.ok()) {
      break;
    }
    drain::SnapshotInfo info;
    auto loaded = persistence.load(info);
    if (!loaded.ok()) {
      break;
    }
    ++round_trips;
  }
  const auto save_finished = std::chrono::steady_clock::now();
  const double save_seconds = std::chrono::duration<double>(save_finished - save_began).count();
  std::printf("snapshot round trips    : %zu\n", round_trips);
  std::printf("round trips/s           : %.1f\n",
              save_seconds > 0.0 ? static_cast<double>(round_trips) / save_seconds : 0.0);
  (void)persistence.reset();
  return completed == resources ? 0 : 1;
}
