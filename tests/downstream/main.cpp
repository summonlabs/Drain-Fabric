// Drain Fabric -- independent downstream consumer.
//
// Uses only the installed public API: it builds a target, closes admission with
// a real drain, answers the evacuation hook the way an adjacent runtime would,
// and asserts that the drain completed with zero remaining protected
// dependencies. If the installed package were incomplete, this program would
// not link or would not run.

#include <chrono>
#include <cstdio>
#include <string>

#include <drain/engine.hpp>
#include <drain/version.hpp>

namespace {

class AdjacentRuntime final : public drain::EvacuationSink {
 public:
  explicit AdjacentRuntime(drain::DrainEngine& engine) : engine_(&engine) {}

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

drain::DrainTarget make_target(const std::string& id, const std::string& domain) {
  return drain::DrainTarget(drain::TargetKind::Resource, *drain::ResourceId::parse(id),
                            *drain::DomainId::parse(domain));
}

}  // namespace

int main() {
  std::printf("drainfabric %s downstream consumer\n", drain::version_string().c_str());

  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  drain::DrainEngine engine(policy, clock);
  AdjacentRuntime adjacent(engine);
  engine.set_sink(&adjacent);

  const drain::TimePoint start = clock.now();
  if (!engine.install_incarnation(drain::IncarnationId{1}, start).ok()) {
    std::printf("FAILED to install an incarnation\n");
    return 1;
  }
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  if (!authority.ok()) {
    std::printf("FAILED to issue authority: %s\n", authority.error().to_string().c_str());
    return 1;
  }

  const auto target = make_target("resource-1", "domain-1");
  if (!engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(100)), start).ok()) {
    std::printf("FAILED to add a resource\n");
    return 1;
  }
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, *drain::HolderId::parse("downstream"),
                         {target});
  auto admitted = engine.admit_obligation(flow, drain::Generation{}, authority->id(), clock.now());
  if (!admitted.ok()) {
    std::printf("FAILED to admit an obligation: %s\n", admitted.error().to_string().c_str());
    return 1;
  }

  drain::DrainRequest request;
  request.targets = {target};
  request.reason = "downstream validation";
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  if (!set.ok()) {
    std::printf("FAILED to request a drain: %s\n", set.error().to_string().c_str());
    return 1;
  }
  const drain::DrainId drain_id = engine.drain_set(*set)->members.front();
  for (int step = 0; step < 32; ++step) {
    clock.advance(std::chrono::seconds(1));
    (void)engine.advance(clock.now());
    const auto record = engine.drain(drain_id);
    if (record.has_value() && drain::is_settled(record->state())) {
      break;
    }
  }

  const auto record = engine.drain(drain_id);
  if (!record.has_value() || record->state() != drain::DrainState::Drained) {
    std::printf("FAILED: drain did not complete\n%s", engine.explain(drain_id).render().c_str());
    return 1;
  }
  const drain::AccountingReport audit = engine.audit(clock.now());
  if (!audit.clean()) {
    std::printf("FAILED: accounting is not closed\n%s", audit.render().c_str());
    return 1;
  }
  std::printf("drain %llu completed against the installed package; audit clean\n",
              static_cast<unsigned long long>(drain_id.value()));
  return 0;
}
