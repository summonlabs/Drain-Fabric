// Drain Fabric example -- correlated multi-resource drain.
//
// Shows failure-domain-aware admission of a drain set, deterministic ordering,
// per-step capacity and redundancy validation, and a rejection when the whole
// set could not be removed without violating a commitment.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "drain/engine.hpp"

namespace {

drain::DrainTarget target(const std::string& id, const std::string& domain) {
  return drain::DrainTarget(drain::TargetKind::Resource, *drain::ResourceId::parse(id),
                            *drain::DomainId::parse(domain));
}

void build(drain::DrainEngine& engine, const std::string& domain, const std::string& prefix,
           std::size_t count, drain::TimePoint now) {
  std::vector<drain::ResourceId> members;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string id = prefix + std::to_string(index);
    const auto resource = target(id, domain);
    (void)engine.add_resource(drain::ResourceNode(resource, drain::CapacityUnits::from(100)), now);
    members.push_back(resource.id());
  }
  for (std::size_t left = 0; left < members.size(); ++left) {
    for (std::size_t right = left + 1; right < members.size(); ++right) {
      (void)engine.add_edge(members[left], members[right], now);
    }
  }
  drain::DiversityGroup group;
  group.id = *drain::GroupId::parse("dg-" + prefix);
  group.domain = *drain::DomainId::parse(domain);
  group.required_available = drain::MemberCount::from(2);
  for (std::size_t index = 0; index < members.size(); ++index) {
    drain::FabricPath path;
    path.id = *drain::ResourceId::parse("p-" + prefix + std::to_string(index));
    path.domain = group.domain;
    path.hops.push_back(members[index]);
    (void)engine.add_path(path, now);
    group.paths.push_back(path.id);
  }
  (void)engine.add_diversity_group(group, now);
}

}  // namespace

int main() {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_concurrent_drains_per_domain(2);
  policy.set_max_drains_in_set(8);
  policy.set_serialize_same_domain_in_set(true);
  drain::DrainEngine engine(policy, clock);

  const drain::TimePoint start = clock.now();
  (void)engine.install_incarnation(drain::IncarnationId{1}, start);
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::minutes(10), start);
  build(engine, "pod-a", "a", 4, start);

  drain::DrainRequest request;
  request.targets = {target("a0", "pod-a"), target("a1", "pod-a")};
  request.reason = "rack power maintenance";
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  std::printf("correlated request accepted: set %llu\n",
              static_cast<unsigned long long>(set.ok() ? set->value() : 0));

  drain::DrainRequest too_many;
  too_many.targets = {target("a0", "pod-a"), target("a1", "pod-a"), target("a2", "pod-a")};
  too_many.authority = authority->id();
  auto rejected = engine.request_drain_set(too_many, clock.now());
  std::printf("three-way correlated request: %s\n",
              rejected.ok() ? "accepted" : rejected.error().to_string().c_str());

  for (int step = 0; step < 120; ++step) {
    clock.advance(std::chrono::seconds(1));
    (void)engine.advance(clock.now());
    bool all_settled = true;
    for (const auto& record : engine.drains()) {
      if (!drain::is_settled(record.state())) {
        all_settled = false;
      }
    }
    if (all_settled) {
      break;
    }
  }

  for (const auto& record : engine.drains()) {
    std::printf("drain %llu target=%s state=%s\n", static_cast<unsigned long long>(record.id().value()),
                record.target().to_string().c_str(), drain::to_string(record.state()));
  }
  const drain::AccountingReport audit = engine.audit(clock.now());
  std::printf("\n%s", audit.render().c_str());
  return audit.clean() ? 0 : 1;
}
