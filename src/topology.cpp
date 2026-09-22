#include "drain/topology.hpp"

#include <algorithm>
#include <limits>

#include "drain/checked.hpp"
#include "drain/digest.hpp"

namespace drain {
namespace {

Result<ResourceId> read_resource_id(const JsonValue& value, std::string_view key) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_string()) {
    return Error(ErrorCode::PersistenceCorrupt, "missing resource id field");
  }
  const auto parsed = ResourceId::parse(found->as_string());
  if (!parsed.has_value()) {
    return Error(ErrorCode::MalformedIdentity, "malformed resource id field");
  }
  return *parsed;
}

Result<DomainId> read_domain_id(const JsonValue& value, std::string_view key) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_string() || found->as_string().empty()) {
    return DomainId{};
  }
  const auto parsed = DomainId::parse(found->as_string());
  if (!parsed.has_value()) {
    return Error(ErrorCode::MalformedIdentity, "malformed failure domain field");
  }
  return *parsed;
}

Result<GroupId> read_group_id(const JsonValue& value, std::string_view key) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_string() || found->as_string().empty()) {
    return Error(ErrorCode::PersistenceCorrupt, "missing group id field");
  }
  const auto parsed = GroupId::parse(found->as_string());
  if (!parsed.has_value()) {
    return Error(ErrorCode::MalformedIdentity, "malformed group id field");
  }
  return *parsed;
}

std::uint64_t read_u64(const JsonValue& value, std::string_view key, std::uint64_t fallback = 0) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_uint(fallback);
}

/// Applies the headroom ratio to a required quantity, rounding up so that a
/// fractional headroom can never be rounded away.
std::uint64_t apply_headroom(std::uint64_t required, std::uint64_t numerator, std::uint64_t denominator) {
  if (denominator == 0) {
    return required;
  }
  const auto product = mul_checked<std::uint64_t>(required, numerator);
  if (!product.has_value()) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return (*product + denominator - 1ULL) / denominator;
}

}  // namespace

const char* to_string(ResourceStatus status) {
  switch (status) {
    case ResourceStatus::InService: return "in-service";
    case ResourceStatus::Unavailable: return "unavailable";
    case ResourceStatus::Failed: return "failed";
  }
  return "unknown";
}

std::optional<ResourceStatus> resource_status_from_string(std::string_view text) {
  if (text == "in-service") return ResourceStatus::InService;
  if (text == "unavailable") return ResourceStatus::Unavailable;
  if (text == "failed") return ResourceStatus::Failed;
  return std::nullopt;
}

ResourceNode::ResourceNode(DrainTarget target, CapacityUnits capacity)
    : target_(std::move(target)), capacity_(capacity) {}

CapacityUnits ResourceNode::available() const noexcept {
  return reserved_ < capacity_ ? CapacityUnits::from(capacity_.value() - reserved_.value())
                               : CapacityUnits::from(0);
}

JsonValue ResourceNode::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("target", drain::to_json(target_));
  value.set("capacity", JsonValue(capacity_.value()));
  value.set("reserved", JsonValue(reserved_.value()));
  value.set("status", JsonValue(std::string(drain::to_string(status_))));
  return value;
}

Result<ResourceNode> ResourceNode::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "resource entry is not an object");
  }
  const JsonValue* target = value.find("target");
  if (target == nullptr) {
    return Error(ErrorCode::PersistenceCorrupt, "resource entry lacks a target");
  }
  auto parsed_target = drain_target_from_json(*target);
  if (!parsed_target.ok()) {
    return Error(ErrorCode::PersistenceCorrupt, "resource target rejected: " + parsed_target.error().message());
  }
  ResourceNode node(*parsed_target, CapacityUnits::from(read_u64(value, "capacity")));
  node.set_reserved(CapacityUnits::from(read_u64(value, "reserved")));
  const auto status = resource_status_from_string(value.get_string("status", "in-service"));
  if (!status.has_value()) {
    return Error(ErrorCode::PersistenceCorrupt, "resource entry has an unknown status");
  }
  node.set_status(*status);
  return node;
}

JsonValue FabricPath::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id.str()));
  value.set("domain", JsonValue(domain.str()));
  JsonValue hops_json = JsonValue::array();
  for (const auto& hop : hops) {
    hops_json.array_ref().push_back(JsonValue(hop.str()));
  }
  value.set("hops", std::move(hops_json));
  return value;
}

Result<FabricPath> FabricPath::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "path entry is not an object");
  }
  FabricPath path;
  auto id = read_resource_id(value, "id");
  if (!id.ok()) {
    return Error(ErrorCode::PersistenceCorrupt, "path entry id rejected");
  }
  path.id = *id;
  auto domain = read_domain_id(value, "domain");
  if (!domain.ok()) {
    return domain.error();
  }
  path.domain = *domain;
  const JsonValue* hops = value.find("hops");
  if (hops == nullptr || !hops->is_array() || hops->size() == 0) {
    return Error(ErrorCode::PersistenceCorrupt, "path entry lacks hops");
  }
  if (hops->size() > kMaxPathHops) {
    return Error(ErrorCode::BoundsExceeded, "path hop count exceeds bound");
  }
  for (const auto& hop : hops->as_array()) {
    if (!hop.is_string()) {
      return Error(ErrorCode::PersistenceCorrupt, "path hop is not a string");
    }
    const auto parsed = ResourceId::parse(hop.as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "malformed path hop id");
    }
    if (std::find(path.hops.begin(), path.hops.end(), *parsed) != path.hops.end()) {
      return Error(ErrorCode::PersistenceCorrupt, "path repeats a hop");
    }
    path.hops.push_back(*parsed);
  }
  return path;
}

JsonValue DiversityGroup::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id.str()));
  value.set("domain", JsonValue(domain.str()));
  value.set("required_available", JsonValue(required_available.value()));
  JsonValue members = JsonValue::array();
  for (const auto& path : paths) {
    members.array_ref().push_back(JsonValue(path.str()));
  }
  value.set("paths", std::move(members));
  return value;
}

Result<DiversityGroup> DiversityGroup::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "diversity group entry is not an object");
  }
  DiversityGroup group;
  auto id = read_group_id(value, "id");
  if (!id.ok()) {
    return id.error();
  }
  group.id = *id;
  auto domain = read_domain_id(value, "domain");
  if (!domain.ok()) {
    return domain.error();
  }
  group.domain = *domain;
  group.required_available = MemberCount::from(read_u64(value, "required_available"));
  if (group.required_available.is_zero()) {
    return Error(ErrorCode::PersistenceCorrupt, "diversity group requires at least one available member");
  }
  const JsonValue* members = value.find("paths");
  if (members == nullptr || !members->is_array() || members->size() == 0) {
    return Error(ErrorCode::PersistenceCorrupt, "diversity group lacks member paths");
  }
  if (members->size() > kMaxPaths) {
    return Error(ErrorCode::BoundsExceeded, "diversity group member count exceeds bound");
  }
  for (const auto& entry : members->as_array()) {
    if (!entry.is_string()) {
      return Error(ErrorCode::PersistenceCorrupt, "diversity group member is not a string");
    }
    const auto parsed = ResourceId::parse(entry.as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "malformed diversity group member id");
    }
    group.paths.push_back(*parsed);
  }
  return group;
}

JsonValue CapacityPool::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id.str()));
  value.set("domain", JsonValue(domain.str()));
  value.set("required", JsonValue(required.value()));
  JsonValue members_json = JsonValue::array();
  for (const auto& member : members) {
    members_json.array_ref().push_back(JsonValue(member.str()));
  }
  value.set("members", std::move(members_json));
  return value;
}

Result<CapacityPool> CapacityPool::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "capacity pool entry is not an object");
  }
  CapacityPool pool;
  auto id = read_group_id(value, "id");
  if (!id.ok()) {
    return id.error();
  }
  pool.id = *id;
  auto domain = read_domain_id(value, "domain");
  if (!domain.ok()) {
    return domain.error();
  }
  pool.domain = *domain;
  pool.required = CapacityUnits::from(read_u64(value, "required"));
  const JsonValue* members = value.find("members");
  if (members == nullptr || !members->is_array() || members->size() == 0) {
    return Error(ErrorCode::PersistenceCorrupt, "capacity pool lacks members");
  }
  if (members->size() > kMaxResources) {
    return Error(ErrorCode::BoundsExceeded, "capacity pool member count exceeds bound");
  }
  for (const auto& entry : members->as_array()) {
    if (!entry.is_string()) {
      return Error(ErrorCode::PersistenceCorrupt, "capacity pool member is not a string");
    }
    const auto parsed = ResourceId::parse(entry.as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "malformed capacity pool member id");
    }
    pool.members.push_back(*parsed);
  }
  return pool;
}

JsonValue ProtectedRoute::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id.str()));
  value.set("domain", JsonValue(domain.str()));
  value.set("origin", JsonValue(origin.str()));
  value.set("terminus", JsonValue(terminus.str()));
  return value;
}

Result<ProtectedRoute> ProtectedRoute::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "protected route entry is not an object");
  }
  ProtectedRoute route;
  auto id = read_group_id(value, "id");
  if (!id.ok()) {
    return id.error();
  }
  route.id = *id;
  auto domain = read_domain_id(value, "domain");
  if (!domain.ok()) {
    return domain.error();
  }
  route.domain = *domain;
  auto origin = read_resource_id(value, "origin");
  if (!origin.ok()) {
    return Error(ErrorCode::PersistenceCorrupt, "protected route origin rejected");
  }
  route.origin = *origin;
  auto terminus = read_resource_id(value, "terminus");
  if (!terminus.ok()) {
    return Error(ErrorCode::PersistenceCorrupt, "protected route terminus rejected");
  }
  route.terminus = *terminus;
  return route;
}

Status Topology::add_resource(ResourceNode node) {
  if (node.id().empty()) {
    return fail(ErrorCode::MalformedIdentity, "resource id must not be empty");
  }
  if (resources_.size() >= kMaxResources) {
    return fail(ErrorCode::BoundsExceeded, "topology resource limit reached");
  }
  if (!resources_.emplace(node.id(), std::move(node)).second) {
    return fail(ErrorCode::AlreadyExists, "resource already present in topology");
  }
  return ok_status();
}

Status Topology::remove_resource(const ResourceId& id) {
  const auto it = resources_.find(id);
  if (it == resources_.end()) {
    return fail(ErrorCode::NotFound, "resource is not present in topology");
  }
  resources_.erase(it);
  adjacency_.erase(id);
  for (auto& [peer, peers] : adjacency_) {
    (void)peer;
    peers.erase(std::remove(peers.begin(), peers.end(), id), peers.end());
  }
  return ok_status();
}

Status Topology::add_edge(const ResourceId& a, const ResourceId& b) {
  if (a == b) {
    return fail(ErrorCode::InvalidArgument, "self edges are not permitted");
  }
  if (resources_.find(a) == resources_.end() || resources_.find(b) == resources_.end()) {
    return fail(ErrorCode::NotFound, "both endpoints of an edge must exist");
  }
  auto& left = adjacency_[a];
  auto& right = adjacency_[b];
  const auto insert_sorted = [](std::vector<ResourceId>& peers, const ResourceId& peer) {
    const auto position = std::lower_bound(peers.begin(), peers.end(), peer);
    if (position == peers.end() || *position != peer) {
      peers.insert(position, peer);
    }
  };
  insert_sorted(left, b);
  insert_sorted(right, a);
  return ok_status();
}

Status Topology::add_path(FabricPath path) {
  if (path.id.empty()) {
    return fail(ErrorCode::MalformedIdentity, "path id must not be empty");
  }
  if (path.hops.empty()) {
    return fail(ErrorCode::InvalidArgument, "path must traverse at least one resource");
  }
  if (path.hops.size() > kMaxPathHops) {
    return fail(ErrorCode::BoundsExceeded, "path hop count exceeds bound");
  }
  if (paths_.size() >= kMaxPaths) {
    return fail(ErrorCode::BoundsExceeded, "topology path limit reached");
  }
  std::set<ResourceId> seen;
  for (const auto& hop : path.hops) {
    if (!seen.insert(hop).second) {
      return fail(ErrorCode::InvalidArgument, "path repeats a hop");
    }
  }
  if (!paths_.emplace(path.id, std::move(path)).second) {
    return fail(ErrorCode::AlreadyExists, "path already present in topology");
  }
  return ok_status();
}

Status Topology::add_diversity_group(DiversityGroup group) {
  if (group.id.empty()) {
    return fail(ErrorCode::MalformedIdentity, "diversity group id must not be empty");
  }
  if (group.paths.empty()) {
    return fail(ErrorCode::InvalidArgument, "diversity group must list at least one path");
  }
  if (group.required_available.is_zero() ||
      group.required_available.value() > group.paths.size()) {
    return fail(ErrorCode::InvalidArgument,
                "diversity requirement must be between one and the number of member paths");
  }
  if (diversity_groups_.size() >= kMaxGroups) {
    return fail(ErrorCode::BoundsExceeded, "diversity group limit reached");
  }
  if (!diversity_groups_.emplace(group.id, std::move(group)).second) {
    return fail(ErrorCode::AlreadyExists, "diversity group already present");
  }
  return ok_status();
}

Status Topology::add_capacity_pool(CapacityPool pool) {
  if (pool.id.empty()) {
    return fail(ErrorCode::MalformedIdentity, "capacity pool id must not be empty");
  }
  if (pool.members.empty()) {
    return fail(ErrorCode::InvalidArgument, "capacity pool must list at least one member");
  }
  if (capacity_pools_.size() >= kMaxGroups) {
    return fail(ErrorCode::BoundsExceeded, "capacity pool limit reached");
  }
  if (!capacity_pools_.emplace(pool.id, std::move(pool)).second) {
    return fail(ErrorCode::AlreadyExists, "capacity pool already present");
  }
  return ok_status();
}

Status Topology::add_protected_route(ProtectedRoute route) {
  if (route.id.empty()) {
    return fail(ErrorCode::MalformedIdentity, "protected route id must not be empty");
  }
  if (route.origin == route.terminus) {
    return fail(ErrorCode::InvalidArgument, "protected route endpoints must differ");
  }
  if (protected_routes_.size() >= kMaxProtectedRoutes) {
    return fail(ErrorCode::BoundsExceeded, "protected route limit reached");
  }
  for (const auto& existing : protected_routes_) {
    if (existing.id == route.id) {
      return fail(ErrorCode::AlreadyExists, "protected route already present");
    }
  }
  protected_routes_.push_back(std::move(route));
  return ok_status();
}

Status Topology::set_status(const ResourceId& id, ResourceStatus status) {
  auto* node = mutable_resource(id);
  if (node == nullptr) {
    return fail(ErrorCode::NotFound, "resource is not present in topology");
  }
  node->set_status(status);
  return ok_status();
}

const ResourceNode* Topology::resource(const ResourceId& id) const {
  const auto it = resources_.find(id);
  return it == resources_.end() ? nullptr : &it->second;
}

ResourceNode* Topology::mutable_resource(const ResourceId& id) {
  const auto it = resources_.find(id);
  return it == resources_.end() ? nullptr : &it->second;
}

const FabricPath* Topology::path(const ResourceId& id) const {
  const auto it = paths_.find(id);
  return it == paths_.end() ? nullptr : &it->second;
}

std::vector<ResourceId> Topology::neighbors(const ResourceId& id) const {
  const auto it = adjacency_.find(id);
  return it == adjacency_.end() ? std::vector<ResourceId>{} : it->second;
}

std::vector<ResourceId> Topology::sorted_resource_ids() const {
  std::vector<ResourceId> out;
  out.reserve(resources_.size());
  for (const auto& [id, node] : resources_) {
    (void)node;
    out.push_back(id);
  }
  return out;
}

bool Topology::path_available(const ResourceId& path_id) const {
  const auto it = paths_.find(path_id);
  if (it == paths_.end()) {
    return false;
  }
  for (const auto& hop : it->second.hops) {
    const auto* node = resource(hop);
    if (node == nullptr || !node->in_service()) {
      return false;
    }
  }
  return true;
}

std::vector<ResourceId> Topology::available_group_paths(const DiversityGroup& group) const {
  std::vector<ResourceId> out;
  for (const auto& path_id : group.paths) {
    if (path_available(path_id)) {
      out.push_back(path_id);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::uint64_t Topology::available_pool_capacity(const CapacityPool& pool,
                                                const std::set<ResourceId>& exclude) const {
  std::uint64_t total = 0;
  for (const auto& member : pool.members) {
    if (exclude.find(member) != exclude.end()) {
      continue;
    }
    const auto* node = resource(member);
    if (node == nullptr || !node->in_service()) {
      continue;
    }
    const auto sum = add_checked<std::uint64_t>(total, node->available().value());
    total = sum.has_value() ? *sum : std::numeric_limits<std::uint64_t>::max();
  }
  return total;
}

std::vector<std::vector<ResourceId>> Topology::enumerate_routes(const ResourceId& from, const ResourceId& to,
                                                               const std::set<ResourceId>& exclude,
                                                               std::size_t max_routes,
                                                               std::size_t max_depth) const {
  std::vector<std::vector<ResourceId>> routes;
  if (max_routes == 0 || max_depth == 0) {
    return routes;
  }
  const std::size_t route_budget = std::min(max_routes, kMaxRoutesPerQuery);
  const std::size_t depth_budget = std::min(max_depth, kMaxRouteDepth);
  if (exclude.find(from) != exclude.end() || exclude.find(to) != exclude.end()) {
    return routes;
  }
  const auto* origin = resource(from);
  const auto* target_node = resource(to);
  if (origin == nullptr || target_node == nullptr || !origin->in_service() || !target_node->in_service()) {
    return routes;
  }

  std::size_t expansions = 0;
  std::vector<ResourceId> stack;
  std::set<ResourceId> on_path;
  stack.push_back(from);
  on_path.insert(from);

  // Iterative depth-first search with an explicit neighbour cursor so that the
  // traversal order matches the sorted adjacency order exactly.
  std::vector<std::size_t> cursor;
  cursor.push_back(0);

  while (!stack.empty()) {
    if (routes.size() >= route_budget || expansions >= kMaxRouteExpansions) {
      break;
    }
    const ResourceId current = stack.back();
    std::size_t& index = cursor.back();
    const auto peers = neighbors(current);
    if (index >= peers.size()) {
      on_path.erase(current);
      stack.pop_back();
      cursor.pop_back();
      continue;
    }
    const ResourceId next = peers[index];
    ++index;
    ++expansions;
    if (on_path.find(next) != on_path.end()) {
      continue;
    }
    const auto* node = resource(next);
    if (node == nullptr || !node->in_service() || exclude.find(next) != exclude.end()) {
      continue;
    }
    if (stack.size() >= depth_budget) {
      continue;
    }
    stack.push_back(next);
    on_path.insert(next);
    cursor.push_back(0);
    if (next == to) {
      routes.push_back(stack);
      on_path.erase(next);
      stack.pop_back();
      cursor.pop_back();
    }
  }
  std::sort(routes.begin(), routes.end());
  routes.erase(std::unique(routes.begin(), routes.end()), routes.end());
  return routes;
}

TopologyViolation Topology::validate_removal(const ResourceId& removing, std::uint64_t headroom_numerator,
                                             std::uint64_t headroom_denominator) const {
  std::set<ResourceId> single;
  single.insert(removing);
  return validate_removal_many(single, headroom_numerator, headroom_denominator);
}

TopologyViolation Topology::validate_removal_many(const std::set<ResourceId>& removing,
                                                  std::uint64_t headroom_numerator,
                                                  std::uint64_t headroom_denominator) const {
  TopologyViolation violation;
  if (removing.empty()) {
    violation.kind = TopologyViolation::Kind::UnknownResource;
    violation.detail = "no resources were named for removal";
    return violation;
  }
  for (const auto& id : removing) {
    const auto* node = resource(id);
    if (node == nullptr) {
      violation.kind = TopologyViolation::Kind::UnknownResource;
      violation.subject = id.str();
      violation.detail = "resource is not present in the topology";
      return violation;
    }
    if (node->reserved() > node->capacity()) {
      violation.kind = TopologyViolation::Kind::ReservationOverCapacity;
      violation.subject = id.str();
      violation.required = node->reserved().value();
      violation.remaining = node->capacity().value();
      violation.detail = "resource reservations exceed its recorded capacity";
      return violation;
    }
  }

  std::set<ResourceId> excluded;
  for (const auto& [id, candidate] : resources_) {
    (void)id;
    if (!candidate.in_service()) {
      excluded.insert(candidate.id());
    }
  }
  excluded.insert(removing.begin(), removing.end());
  const std::string removing_label =
      removing.size() == 1 ? removing.begin()->str() : std::string("drain-set");

  // Capacity commitments, checked in deterministic group order.
  for (const auto& [pool_id, pool] : capacity_pools_) {
    const std::uint64_t remaining = available_pool_capacity(pool, excluded);
    const std::uint64_t required = apply_headroom(pool.required.value(), headroom_numerator, headroom_denominator);
    if (remaining < required) {
      violation.kind = TopologyViolation::Kind::CapacityPoolExhausted;
      violation.subject = pool_id.str();
      violation.required = required;
      violation.remaining = remaining;
      violation.detail = "removing " + removing_label + " would leave the pool below its commitment";
      return violation;
    }
  }

  // Redundancy commitments, checked in deterministic group order.
  for (const auto& [group_id, group] : diversity_groups_) {
    std::size_t available = 0;
    for (const auto& path_id : group.paths) {
      const auto it = paths_.find(path_id);
      if (it == paths_.end()) {
        continue;
      }
      bool usable = true;
      for (const auto& hop : it->second.hops) {
        if (excluded.find(hop) != excluded.end()) {
          usable = false;
          break;
        }
      }
      if (usable) {
        ++available;
      }
    }
    if (available < group.required_available.value()) {
      violation.kind = TopologyViolation::Kind::DiversityLost;
      violation.subject = group_id.str();
      violation.required = group.required_available.value();
      violation.remaining = available;
      violation.detail = "removing " + removing_label + " would drop available paths below the commitment";
      return violation;
    }
  }

  // Connectivity commitments, checked in declaration order (which is stable).
  for (const auto& route : protected_routes_) {
    const auto candidates = enumerate_routes(route.origin, route.terminus, excluded, 1, kMaxRouteDepth);
    if (candidates.empty()) {
      violation.kind = TopologyViolation::Kind::RouteSevered;
      violation.subject = route.id.str();
      violation.required = 1;
      violation.remaining = 0;
      violation.detail = "no route remains between " + route.origin.str() + " and " + route.terminus.str();
      return violation;
    }
  }
  return violation;
}

JsonValue Topology::to_json() const {
  JsonValue value = JsonValue::object();
  JsonValue resources = JsonValue::array();
  for (const auto& [id, node] : resources_) {
    (void)id;
    resources.array_ref().push_back(node.to_json());
  }
  value.set("resources", std::move(resources));

  JsonValue edges = JsonValue::array();
  for (const auto& [from, peers] : adjacency_) {
    for (const auto& peer : peers) {
      if (peer < from) {
        continue;  // each undirected edge is emitted once
      }
      JsonValue edge = JsonValue::object();
      edge.set("from", JsonValue(from.str()));
      edge.set("to", JsonValue(peer.str()));
      edges.array_ref().push_back(std::move(edge));
    }
  }
  value.set("edges", std::move(edges));

  JsonValue paths = JsonValue::array();
  for (const auto& [id, path] : paths_) {
    (void)id;
    paths.array_ref().push_back(path.to_json());
  }
  value.set("paths", std::move(paths));

  JsonValue groups = JsonValue::array();
  for (const auto& [id, group] : diversity_groups_) {
    (void)id;
    groups.array_ref().push_back(group.to_json());
  }
  value.set("diversity_groups", std::move(groups));

  JsonValue pools = JsonValue::array();
  for (const auto& [id, pool] : capacity_pools_) {
    (void)id;
    pools.array_ref().push_back(pool.to_json());
  }
  value.set("capacity_pools", std::move(pools));

  JsonValue routes = JsonValue::array();
  for (const auto& route : protected_routes_) {
    routes.array_ref().push_back(route.to_json());
  }
  value.set("protected_routes", std::move(routes));
  return value;
}

Status Topology::load_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return fail(ErrorCode::PersistenceCorrupt, "topology payload is not an object");
  }
  Topology staged;

  const JsonValue* resources = value.find("resources");
  if (resources == nullptr || !resources->is_array()) {
    return fail(ErrorCode::PersistenceCorrupt, "topology payload lacks a resources array");
  }
  if (resources->size() > kMaxResources) {
    return fail(ErrorCode::BoundsExceeded, "topology resource count exceeds bound");
  }
  for (const auto& entry : resources->as_array()) {
    auto node = ResourceNode::from_json(entry);
    if (!node.ok()) {
      return fail(ErrorCode::PersistenceCorrupt, "resource entry rejected: " + node.error().message());
    }
    Status added = staged.add_resource(*node);
    if (!added.ok()) {
      return fail(ErrorCode::PersistenceCorrupt, "resource entry rejected: " + added.error().message());
    }
  }

  const JsonValue* paths = value.find("paths");
  if (paths != nullptr && paths->is_array()) {
    if (paths->size() > kMaxPaths) {
      return fail(ErrorCode::BoundsExceeded, "topology path count exceeds bound");
    }
    for (const auto& entry : paths->as_array()) {
      auto path = FabricPath::from_json(entry);
      if (!path.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "path entry rejected: " + path.error().message());
      }
      Status added = staged.add_path(*path);
      if (!added.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "path entry rejected: " + added.error().message());
      }
    }
  }

  const JsonValue* edges = value.find("edges");
  if (edges != nullptr && edges->is_array()) {
    if (edges->size() > kMaxResources * kMaxResources) {
      return fail(ErrorCode::BoundsExceeded, "topology edge count exceeds bound");
    }
    for (const auto& entry : edges->as_array()) {
      auto from = read_resource_id(entry, "from");
      if (!from.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "edge entry rejected");
      }
      auto to = read_resource_id(entry, "to");
      if (!to.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "edge entry rejected");
      }
      Status added = staged.add_edge(*from, *to);
      if (!added.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "edge entry rejected: " + added.error().message());
      }
    }
  }

  const JsonValue* groups = value.find("diversity_groups");
  if (groups != nullptr && groups->is_array()) {
    if (groups->size() > kMaxGroups) {
      return fail(ErrorCode::BoundsExceeded, "diversity group count exceeds bound");
    }
    for (const auto& entry : groups->as_array()) {
      auto group = DiversityGroup::from_json(entry);
      if (!group.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "diversity group rejected: " + group.error().message());
      }
      Status added = staged.add_diversity_group(*group);
      if (!added.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "diversity group rejected: " + added.error().message());
      }
    }
  }

  const JsonValue* pools = value.find("capacity_pools");
  if (pools != nullptr && pools->is_array()) {
    if (pools->size() > kMaxGroups) {
      return fail(ErrorCode::BoundsExceeded, "capacity pool count exceeds bound");
    }
    for (const auto& entry : pools->as_array()) {
      auto pool = CapacityPool::from_json(entry);
      if (!pool.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "capacity pool rejected: " + pool.error().message());
      }
      Status added = staged.add_capacity_pool(*pool);
      if (!added.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "capacity pool rejected: " + added.error().message());
      }
    }
  }

  const JsonValue* routes = value.find("protected_routes");
  if (routes != nullptr && routes->is_array()) {
    if (routes->size() > kMaxProtectedRoutes) {
      return fail(ErrorCode::BoundsExceeded, "protected route count exceeds bound");
    }
    for (const auto& entry : routes->as_array()) {
      auto route = ProtectedRoute::from_json(entry);
      if (!route.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "protected route rejected: " + route.error().message());
      }
      Status added = staged.add_protected_route(*route);
      if (!added.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "protected route rejected: " + added.error().message());
      }
    }
  }

  *this = std::move(staged);
  return ok_status();
}

void Topology::clear() {
  resources_.clear();
  paths_.clear();
  diversity_groups_.clear();
  capacity_pools_.clear();
  protected_routes_.clear();
  adjacency_.clear();
}

}  // namespace drain
