#pragma once

// Drain Fabric -- strongly typed domain identities.
//
// Every domain object that can be confused with another kind of object carries a
// distinct C++ type. String-shaped identities are validated slugs; numeric
// identities (generation, attempt, epoch, incarnation, authority, sequence) are
// distinct template instantiations so that they cannot be interchanged, compared,
// or accidentally assigned to one another.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "drain/export.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"

namespace drain {

/// Maximum accepted length of a slug identity.
inline constexpr std::size_t kMaxSlugLength = 128;
/// Maximum number of targets accepted in a single request (bounded payload).
inline constexpr std::size_t kMaxTargetsPerRequest = 256;

/// A slug is the accepted textual form of a string identity: non-empty, at most
/// kMaxSlugLength characters, drawn from [A-Za-z0-9_.:-]. The restricted alphabet
/// keeps identity parsing, serialization, and CLI quoting unambiguous.
DRAIN_API bool is_valid_slug(std::string_view text);

/// Strongly typed slug identity. Tag makes every instantiation a distinct type.
template <class Tag>
class SlugId {
 public:
  SlugId() = default;
  explicit SlugId(std::string value) : value_(std::move(value)) {}

  static std::optional<SlugId> parse(std::string_view text) {
    if (!is_valid_slug(text)) {
      return std::nullopt;
    }
    return SlugId(std::string(text));
  }

  const std::string& str() const noexcept { return value_; }
  bool empty() const noexcept { return value_.empty(); }
  std::size_t size() const noexcept { return value_.size(); }

  friend bool operator==(const SlugId&, const SlugId&) noexcept = default;
  friend auto operator<=>(const SlugId&, const SlugId&) noexcept = default;

 private:
  std::string value_;
};

struct ResourceTag;
struct DomainTag;
struct HolderTag;
struct GroupTag;
struct ServiceTag;
struct NodeTag;

using ResourceId = SlugId<ResourceTag>;
using DomainId = SlugId<DomainTag>;
using HolderId = SlugId<HolderTag>;
using GroupId = SlugId<GroupTag>;
using ServiceId = SlugId<ServiceTag>;
using NodeId = SlugId<NodeTag>;

/// Strongly typed numeric identity. Rep is the storage type (checked arithmetic
/// applies wherever a value is derived from external input).
template <class Tag, class Rep = std::uint64_t>
class NumId {
 public:
  using rep_type = Rep;

  constexpr NumId() noexcept = default;
  constexpr explicit NumId(Rep value) noexcept : value_(value) {}

  constexpr Rep value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == Rep{0}; }
  constexpr NumId next() const noexcept { return NumId(static_cast<Rep>(value_ + Rep{1})); }

  friend constexpr bool operator==(NumId, NumId) noexcept = default;
  friend constexpr auto operator<=>(NumId, NumId) noexcept = default;

 private:
  Rep value_{0};
};

/// Strongly typed non-negative quantity. Capacities, member counts, and byte
/// budgets are distinct types so that a capacity can never be compared with, or
/// assigned to, an unrelated count.
template <class Tag, class Rep = std::uint64_t>
class Quantity {
 public:
  using rep_type = Rep;

  constexpr Quantity() noexcept = default;
  constexpr explicit Quantity(Rep value) noexcept : value_(value) {}
  constexpr static Quantity from(Rep value) noexcept { return Quantity(value); }

  constexpr Rep value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == Rep{0}; }

  friend constexpr bool operator==(Quantity, Quantity) noexcept = default;
  friend constexpr auto operator<=>(Quantity, Quantity) noexcept = default;

 private:
  Rep value_{0};
};

struct CapacityTag;
struct CountTag;
struct ByteBudgetTag;

using CapacityUnits = Quantity<CapacityTag>;
using MemberCount = Quantity<CountTag>;
using ByteCount = Quantity<ByteBudgetTag>;

struct DrainTag;
struct DrainSetTag;
struct ObligationTag;
struct AttemptTag;
struct GenerationTag;
struct EpochTag;
struct IncarnationTag;
struct AuthorityTag;
struct EvidenceTag;
struct RevisionTag;
struct SequenceTag;
struct NonceTag;

using DrainId = NumId<DrainTag>;
using DrainSetId = NumId<DrainSetTag>;
using ObligationId = NumId<ObligationTag>;
using Attempt = NumId<AttemptTag, std::uint32_t>;
using Generation = NumId<GenerationTag>;
using Epoch = NumId<EpochTag>;
using IncarnationId = NumId<IncarnationTag>;
using AuthorityId = NumId<AuthorityTag>;
using EvidenceSeq = NumId<EvidenceTag>;
using Revision = NumId<RevisionTag>;
using Sequence = NumId<SequenceTag>;
using RequestNonce = NumId<NonceTag>;

/// Kind of fabric resource that can be drained.
enum class TargetKind : std::uint8_t {
  Port = 0,
  Link = 1,
  Switch = 2,
  Path = 3,
  Service = 4,
  Resource = 5,
};

DRAIN_API const char* to_string(TargetKind kind);
DRAIN_API std::optional<TargetKind> target_kind_from_string(std::string_view text);

/// A fully qualified drain target: what is being drained, its identity, and the
/// failure domain it belongs to. The domain participates in correlated-drain
/// admission (failure-domain-aware limits) and deterministic ordering.
class DrainTarget {
 public:
  DrainTarget() = default;
  DrainTarget(TargetKind kind, ResourceId id, DomainId domain);

  TargetKind kind() const noexcept { return kind_; }
  const ResourceId& id() const noexcept { return id_; }
  const DomainId& domain() const noexcept { return domain_; }
  void set_domain(DomainId domain) noexcept { domain_ = std::move(domain); }

  /// Canonical textual form: "<kind>:<id>@<domain>". Deterministic and total.
  std::string to_string() const;

  /// Parses "<kind>:<id>" or "<kind>:<id>@<domain>".
  static std::optional<DrainTarget> parse(std::string_view text);

  friend bool operator==(const DrainTarget&, const DrainTarget&) noexcept = default;
  friend auto operator<=>(const DrainTarget&, const DrainTarget&) noexcept = default;

 private:
  TargetKind kind_{TargetKind::Resource};
  ResourceId id_{};
  DomainId domain_{};
};

/// Deterministic total order used for drain-set ordering and explanation output:
/// failure domain first (limits are domain scoped), then kind rank, then identity.
DRAIN_API bool target_order_less(const DrainTarget& a, const DrainTarget& b);

/// Canonical JSON form of a target. Round trips through drain_target_from_json.
DRAIN_API JsonValue to_json(const DrainTarget& target);
DRAIN_API Result<DrainTarget> drain_target_from_json(const JsonValue& value);

DRAIN_API std::string to_string(const ResourceId& id);
DRAIN_API std::string to_string(const DomainId& id);
DRAIN_API std::string to_string(const HolderId& id);
DRAIN_API std::string to_string(const GroupId& id);
DRAIN_API std::string to_string(const ServiceId& id);
DRAIN_API std::string to_string(const NodeId& id);

template <class Tag, class Rep>
std::string to_string(const NumId<Tag, Rep>& id) {
  return std::to_string(static_cast<unsigned long long>(id.value()));
}

}  // namespace drain

namespace std {

template <class Tag>
struct hash<drain::SlugId<Tag>> {
  std::size_t operator()(const drain::SlugId<Tag>& id) const noexcept {
    return std::hash<std::string>{}(id.str());
  }
};

template <class Tag, class Rep>
struct hash<drain::NumId<Tag, Rep>> {
  std::size_t operator()(const drain::NumId<Tag, Rep>& id) const noexcept {
    return std::hash<Rep>{}(id.value());
  }
};

template <class Tag, class Rep>
struct hash<drain::Quantity<Tag, Rep>> {
  std::size_t operator()(const drain::Quantity<Tag, Rep>& quantity) const noexcept {
    return std::hash<Rep>{}(quantity.value());
  }
};

template <>
struct hash<drain::DrainTarget> {
  std::size_t operator()(const drain::DrainTarget& target) const noexcept {
    std::size_t h = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(target.kind()));
    h ^= std::hash<drain::ResourceId>{}(target.id()) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<drain::DomainId>{}(target.domain()) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};

}  // namespace std
