// Drain Fabric example -- one complete drain lifecycle.
//
// Shows the whole boundary: close admission, request evacuation from an adjacent
// runtime, validate capacity and redundancy before the removal step, prove zero
// remaining protected dependencies, and explain every decision that was taken.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "drain/engine.hpp"

namespace {

/// A deliberately tiny adjacent runtime. It is not part of Drain Fabric: it
/// receives evacuation requests and answers them by recording release evidence
/// and reporting the obligation state back through the public API.
class AdjacentRuntime final : public drain::EvacuationSink {
 public:
  explicit AdjacentRuntime(drain::DrainEngine& engine) : engine_(&engine) {}

  std::size_t requests() const noexcept { return requests_; }

  drain::Status on_evacuation_request(const drain::EvacuationRequest& request) override {
    ++requests_;
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
    evidence.detail = "adjacent runtime rerouted the flow";
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
  std::size_t requests_{0};
};

drain::DrainTarget target(const std::string& id, const std::string& domain) {
  return drain::DrainTarget(drain::TargetKind::Resource, *drain::ResourceId::parse(id),
                            *drain::DomainId::parse(domain));
}

}  // namespace

int main() {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(1));
  policy.set_evidence_freshness(std::chrono::seconds(60));
  drain::DrainEngine engine(policy, clock);

  AdjacentRuntime adjacent(engine);
  engine.set_sink(&adjacent);

  const drain::TimePoint start = clock.now();
  (void)engine.install_incarnation(drain::IncarnationId{1}, start);
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::minutes(10), start);
  if (!authority.ok()) {
    std::printf("cannot issue authority: %s\n", authority.error().to_string().c_str());
    return 1;
  }

  const auto spine = target("spine-1", "pod-a");
  const auto leaf = target("leaf-1", "pod-a");
  (void)engine.add_resource(drain::ResourceNode(spine, drain::CapacityUnits::from(100)), start);
  (void)engine.add_resource(drain::ResourceNode(leaf, drain::CapacityUnits::from(100)), start);
  (void)engine.add_edge(spine.id(), leaf.id(), start);

  drain::DiversityGroup group;
  group.id = *drain::GroupId::parse("dg-a");
  group.domain = *drain::DomainId::parse("pod-a");
  group.required_available = drain::MemberCount::from(1);
  drain::FabricPath direct;
  direct.id = *drain::ResourceId::parse("p-direct");
  direct.domain = group.domain;
  direct.hops.push_back(leaf.id());
  (void)engine.add_path(direct, start);
  drain::FabricPath via;
  via.id = *drain::ResourceId::parse("p-via");
  via.domain = group.domain;
  via.hops.push_back(spine.id());
  via.hops.push_back(leaf.id());
  (void)engine.add_path(via, start);
  group.paths.push_back(direct.id);
  group.paths.push_back(via.id);
  (void)engine.add_diversity_group(group, start);

  drain::CapacityPool pool;
  pool.id = *drain::GroupId::parse("cp-a");
  pool.domain = group.domain;
  pool.required = drain::CapacityUnits::from(40);
  pool.members.push_back(spine.id());
  pool.members.push_back(leaf.id());
  (void)engine.add_capacity_pool(pool, start);

  drain::Obligation flow(drain::ObligationKind::ActiveFlow, *drain::HolderId::parse("workload-7"),
                         {spine, leaf});
  flow.set_service(*drain::ServiceId::parse("analytics"));
  auto admitted = engine.admit_obligation(flow, drain::Generation{}, authority->id(), clock.now());
  if (!admitted.ok()) {
    std::printf("admission failed: %s\n", admitted.error().to_string().c_str());
    return 1;
  }
  std::printf("admitted %s\n", admitted->label().c_str());

  drain::DrainRequest request;
  request.targets = {spine};
  request.reason = "firmware maintenance window";
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  if (!set.ok()) {
    std::printf("request rejected: %s\n", set.error().to_string().c_str());
    return 1;
  }
  const auto stored = engine.drain_set(*set);
  const drain::DrainId drain_id = stored->members.front();
  std::printf("requested drain %llu\n", static_cast<unsigned long long>(drain_id.value()));

  for (int step = 0; step < 40; ++step) {
    clock.advance(std::chrono::seconds(1));
    (void)engine.advance(clock.now());
    const auto record = engine.drain(drain_id);
    if (!record.has_value() || drain::is_settled(record->state())) {
      break;
    }
  }

  const auto record = engine.drain(drain_id);
  std::printf("\nfinal state: %s\n", drain::to_string(record->state()));
  std::printf("remaining protected dependencies at completion: %u\n",
              record->remaining_protected_at_completion());
  std::printf("\n--- explanation ---\n%s", engine.explain(drain_id).render().c_str());

  const drain::AccountingReport audit = engine.audit(clock.now());
  std::printf("\n--- audit ---\n%s", audit.render().c_str());
  std::printf("\nadjacent runtime answered %zu evacuation request(s)\n", adjacent.requests());
  return audit.clean() ? 0 : 1;
}
