#pragma once

// Drain Fabric -- fabric topology model.
//
// The topology is the set of resources Drain Fabric may drain, the declared
// paths over them, and the redundancy / capacity / connectivity commitments that
// constrain correlated removals. It is deliberately declarative: Drain Fabric
// validates removal steps against it and never recomputes routing policy.
//
// Every collection is bounded and every iteration order is deterministic.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"

namespace drain {

/// Bounds on topology size. Externally supplied topology documents are checked
/// against these before anything is allocated.
inline constexpr std::size_t kMaxResources = 4096;
inline constexpr std::size_t kMaxPaths = 16384;
inline constexpr std::size_t kMaxPathHops = 64;
inline constexpr std::size_t kMaxGroups = 1024;
inline constexpr std::size_t kMaxProtectedRoutes = 1024;
inline constexpr std::size_t kMaxRoutesPerQuery = 8;
inline constexpr std::size_t kMaxRouteDepth = 16;
inline constexpr std::size_t kMaxRouteExpansions = 20000;

/// Administrative status of a resource. Drain Fabric owns the Unavailable
/// transition; Failed models an adjacent-runtime report that a resource is gone.
enum class ResourceStatus : std::uint8_t { InService = 0, Unavailable = 1, Failed = 2 };

DRAIN_API const char* to_string(ResourceStatus status);
DRAIN_API std::optional<ResourceStatus> resource_status_from_string(std::string_view text);

/// A drainable resource and its capacity ledger.
class ResourceNode {
 public:
  ResourceNode() = default;
  ResourceNode(DrainTarget target, CapacityUnits capacity);

  const DrainTarget& target() const noexcept { return target_; }
  const ResourceId& id() const noexcept { return target_.id(); }
  const DomainId& domain() const noexcept { return target_.domain(); }

  CapacityUnits capacity() const noexcept { return capacity_; }
  void set_capacity(CapacityUnits capacity) noexcept { capacity_ = capacity; }

  CapacityUnits reserved() const noexcept { return reserved_; }
  void set_reserved(CapacityUnits reserved) noexcept { reserved_ = reserved; }

  /// Capacity still available to new work. Saturates at zero when reservations
  /// exceed the recorded capacity, which the validator reports separately.
  CapacityUnits available() const noexcept;

  ResourceStatus status() const noexcept { return status_; }
  void set_status(ResourceStatus status) noexcept { status_ = status; }

  bool in_service() const noexcept { return status_ == ResourceStatus::InService; }

  JsonValue to_json() const;
  static Result<ResourceNode> from_json(const JsonValue& value);

 private:
  DrainTarget target_{};
  CapacityUnits capacity_{};
  CapacityUnits reserved_{};
  ResourceStatus status_{ResourceStatus::InService};
};

/// A declared path: an ordered set of resources that carries traffic between two
/// endpoints. Paths are named so that explanations can cite them.
struct FabricPath {
  ResourceId id{};
  DomainId domain{};
  std::vector<ResourceId> hops{};

  JsonValue to_json() const;
  static Result<FabricPath> from_json(const JsonValue& value);
};

/// A redundancy commitment: at least required_available of the listed paths
/// must be in service inside the group domain.
struct DiversityGroup {
  GroupId id{};
  DomainId domain{};
  MemberCount required_available{};
  std::vector<ResourceId> paths{};

  JsonValue to_json() const;
  static Result<DiversityGroup> from_json(const JsonValue& value);
};

/// A capacity commitment: at least required capacity units must remain available
/// across the listed resources.
struct CapacityPool {
  GroupId id{};
  DomainId domain{};
  CapacityUnits required{};
  std::vector<ResourceId> members{};

  JsonValue to_json() const;
  static Result<CapacityPool> from_json(const JsonValue& value);
};

/// A connectivity commitment: an alternate route must exist between origin and
/// terminus. Checked before a removal step that could sever the last route.
struct ProtectedRoute {
  GroupId id{};
  DomainId domain{};
  ResourceId origin{};
  ResourceId terminus{};

  JsonValue to_json() const;
  static Result<ProtectedRoute> from_json(const JsonValue& value);
};

/// Result of a removal-step validation. Failures carry the exact commitment
/// that would be violated.
struct TopologyViolation {
  enum class Kind : std::uint8_t {
    None = 0,
    UnknownResource = 1,
    CapacityPoolExhausted = 2,
    DiversityLost = 3,
    RouteSevered = 4,
    ReservationOverCapacity = 5,
  };

  Kind kind{Kind::None};
  std::string subject{};
  std::string detail{};
  std::uint64_t required{0};
  std::uint64_t remaining{0};

  bool ok() const noexcept { return kind == Kind::None; }
};

class Topology {
 public:
  Status add_resource(ResourceNode node);
  Status remove_resource(const ResourceId& id);

  /// Adds an undirected adjacency edge. Self edges and duplicate edges are
  /// rejected.
  Status add_edge(const ResourceId& a, const ResourceId& b);

  Status add_path(FabricPath path);
  Status add_diversity_group(DiversityGroup group);
  Status add_capacity_pool(CapacityPool pool);
  Status add_protected_route(ProtectedRoute route);

  Status set_status(const ResourceId& id, ResourceStatus status);

  const ResourceNode* resource(const ResourceId& id) const;
  ResourceNode* mutable_resource(const ResourceId& id);
  const FabricPath* path(const ResourceId& id) const;

  std::vector<ResourceId> neighbors(const ResourceId& id) const;
  std::vector<ResourceId> sorted_resource_ids() const;

  const std::map<ResourceId, ResourceNode>& resources() const noexcept { return resources_; }
  const std::map<ResourceId, FabricPath>& paths() const noexcept { return paths_; }
  const std::map<GroupId, DiversityGroup>& diversity_groups() const noexcept { return diversity_groups_; }
  const std::map<GroupId, CapacityPool>& capacity_pools() const noexcept { return capacity_pools_; }
  const std::vector<ProtectedRoute>& protected_routes() const noexcept { return protected_routes_; }

  std::size_t resource_count() const noexcept { return resources_.size(); }

  /// A declared path is available when every hop is present and in service.
  bool path_available(const ResourceId& path_id) const;

  /// Paths of a group that are currently available, in deterministic order.
  std::vector<ResourceId> available_group_paths(const DiversityGroup& group) const;

  /// Available capacity of a pool, restricted to in-service members and
  /// excluding every resource in the exclusion set.
  std::uint64_t available_pool_capacity(const CapacityPool& pool, const std::set<ResourceId>& exclude) const;

  /// Bounded, deterministic, depth-first route enumeration over the adjacency
  /// graph, avoiding the resources in the exclusion set. Neighbours are visited
  /// in sorted order, so the result depends only on the topology, never on
  /// insertion order. Enumeration stops after kMaxRouteExpansions expansions.
  std::vector<std::vector<ResourceId>> enumerate_routes(const ResourceId& from, const ResourceId& to,
                                                        const std::set<ResourceId>& exclude,
                                                        std::size_t max_routes,
                                                        std::size_t max_depth) const;

  /// Validates a hypothetical removal of one resource (in addition to
  /// everything already marked Unavailable or Failed). Returns the first
  /// violation in canonical order, or an ok violation.
  TopologyViolation validate_removal(const ResourceId& removing, std::uint64_t headroom_numerator,
                                     std::uint64_t headroom_denominator) const;

  /// Validates a correlated removal of several resources at once. Used to admit
  /// a whole drain set before any of its members closes admission, so a set can
  /// never be accepted that would violate a commitment when fully applied.
  TopologyViolation validate_removal_many(const std::set<ResourceId>& removing,
                                          std::uint64_t headroom_numerator,
                                          std::uint64_t headroom_denominator) const;

  JsonValue to_json() const;
  Status load_from_json(const JsonValue& value);

  void clear();

 private:
  std::map<ResourceId, ResourceNode> resources_{};
  std::map<ResourceId, FabricPath> paths_{};
  std::map<GroupId, DiversityGroup> diversity_groups_{};
  std::map<GroupId, CapacityPool> capacity_pools_{};
  std::vector<ProtectedRoute> protected_routes_{};
  std::map<ResourceId, std::vector<ResourceId>> adjacency_{};
};

}  // namespace drain
