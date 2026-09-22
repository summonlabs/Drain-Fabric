// Drain Fabric -- unit tests.
//
// Each case pins an explicit contract of one component. Deterministic
// explanations, strong identity typing, the state machine legality table, the
// evidence ledger fences, the authority line, and the admission boundary are all
// covered here rather than left to the randomized suites.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "drain/admission.hpp"
#include "drain/authority.hpp"
#include "drain/checked.hpp"
#include "drain/clock.hpp"
#include "drain/crc32c.hpp"
#include "drain/decision.hpp"
#include "drain/digest.hpp"
#include "drain/drain_set.hpp"
#include "drain/engine.hpp"
#include "drain/evidence.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/lifecycle.hpp"
#include "drain/obligation.hpp"
#include "drain/persistence.hpp"
#include "drain/policy.hpp"
#include "drain/topology.hpp"
#include "fixture.hpp"
#include "process.hpp"
#include "test.hpp"

using namespace drain_test;

namespace {

/// The engine is deliberately non-copyable and non-movable, so fixtures hold it
/// through a unique_ptr rather than returning it by value.
std::unique_ptr<drain::DrainEngine> make_engine(drain::DrainPolicy& policy, drain::ManualClock& clock) {
  auto engine = std::make_unique<drain::DrainEngine>(policy, clock);
  (void)engine->install_incarnation(drain::IncarnationId{1}, clock.now());
  return engine;
}

}  // namespace

// ---------------------------------------------------------------------------
// identity and checked arithmetic
// ---------------------------------------------------------------------------

DRAIN_TEST(identity, slug_validation) {
  DRAIN_CHECK(drain::is_valid_slug("pod-a_1.2:x"));
  DRAIN_CHECK(!drain::is_valid_slug(""));
  DRAIN_CHECK(!drain::is_valid_slug("has space"));
  DRAIN_CHECK(!drain::is_valid_slug("slash/name"));
  DRAIN_CHECK(!drain::is_valid_slug(std::string(drain::kMaxSlugLength + 1, 'a')));
  DRAIN_CHECK(drain::is_valid_slug(std::string(drain::kMaxSlugLength, 'a')));
}

DRAIN_TEST(identity, target_round_trip) {
  const auto parsed = drain::DrainTarget::parse("port:eth0@podium-a");
  DRAIN_CHECK(parsed.has_value());
  DRAIN_CHECK_EQ(std::string(drain::to_string(parsed->kind())), std::string("port"));
  DRAIN_CHECK_EQ(parsed->id().str(), std::string("eth0"));
  DRAIN_CHECK_EQ(parsed->domain().str(), std::string("podium-a"));
  DRAIN_CHECK_EQ(parsed->to_string(), std::string("port:eth0@podium-a"));
  DRAIN_CHECK(!drain::DrainTarget::parse("port:eth0@bad domain").has_value());
  DRAIN_CHECK(!drain::DrainTarget::parse("nonsense:eth0").has_value());
  DRAIN_CHECK(!drain::DrainTarget::parse("port").has_value());
}

DRAIN_TEST(identity, target_json_round_trip) {
  const auto target = target_of("r1", "d1");
  const drain::JsonValue encoded = drain::to_json(target);
  auto decoded = drain::drain_target_from_json(encoded);
  DRAIN_CHECK(decoded.ok());
  DRAIN_CHECK(*decoded == target);
  auto rejected = drain::drain_target_from_json(drain::JsonValue(std::string("not an object")));
  DRAIN_CHECK(!rejected.ok());
  DRAIN_CHECK_EQ(rejected.error().code(), drain::ErrorCode::ProtocolError);
}

DRAIN_TEST(identity, deterministic_target_order) {
  const auto a = target_of("b", "d1");
  const auto b = target_of("a", "d1");
  const auto c = target_of("a", "d0");
  DRAIN_CHECK(drain::target_order_less(c, a));
  DRAIN_CHECK(drain::target_order_less(b, a));
  DRAIN_CHECK(!drain::target_order_less(a, b));
  std::vector<drain::DrainTarget> targets{a, b, c};
  std::sort(targets.begin(), targets.end(), drain::target_order_less);
  DRAIN_CHECK_EQ(targets[0].to_string(), std::string("resource:a@d0"));
  DRAIN_CHECK_EQ(targets[1].to_string(), std::string("resource:a@d1"));
  DRAIN_CHECK_EQ(targets[2].to_string(), std::string("resource:b@d1"));
}

DRAIN_TEST(checked, overflow_is_reported) {
  const auto sum = drain::add_checked<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(), 1);
  DRAIN_CHECK(!sum.has_value());
  const auto product = drain::mul_checked<std::uint64_t>(1ULL << 63, 4);
  DRAIN_CHECK(!product.has_value());
  DRAIN_CHECK_EQ(*drain::add_checked<std::uint32_t>(2, 3), 5u);
  DRAIN_CHECK(!drain::sub_checked<std::uint32_t>(1, 2).has_value());
  DRAIN_CHECK_EQ(*drain::increment_checked<std::uint64_t>(7), 8u);
  DRAIN_CHECK(!drain::increment_checked<std::uint64_t>(std::numeric_limits<std::uint64_t>::max())
                   .has_value());
  DRAIN_CHECK(!drain::narrow_checked<std::uint8_t>(300).has_value());
  DRAIN_CHECK_EQ(*drain::narrow_checked<std::uint8_t>(300 - 45), 255);
  DRAIN_CHECK(!drain::narrow_checked<std::uint32_t>(-1).has_value());
}

DRAIN_TEST(checked, scale_ratio_checked) {
  DRAIN_CHECK_EQ(*drain::scale_ratio_checked<std::uint64_t>(100, 3, 2), 150u);
  DRAIN_CHECK(!drain::scale_ratio_checked<std::uint64_t>(100, 1, 0).has_value());
  DRAIN_CHECK(!drain::scale_ratio_checked<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(), 2, 1)
                   .has_value());
}

// ---------------------------------------------------------------------------
// clock, digest, crc
// ---------------------------------------------------------------------------

DRAIN_TEST(clock, manual_clock_is_deterministic) {
  drain::ManualClock clock(drain::TimePoint{1000});
  DRAIN_CHECK_EQ(clock.now().count(), 1000);
  clock.advance(std::chrono::nanoseconds(500));
  DRAIN_CHECK_EQ(clock.now().count(), 1500);
  clock.set(drain::TimePoint{0});
  DRAIN_CHECK_EQ(clock.now().count(), 0);
  const drain::TimePoint saturated = drain::saturating_add(
      drain::TimePoint{std::numeric_limits<std::int64_t>::max()}, std::chrono::nanoseconds(5));
  DRAIN_CHECK_EQ(saturated.count(), std::numeric_limits<std::int64_t>::max());
}

DRAIN_TEST(clock, duration_rendering_is_stable) {
  DRAIN_CHECK_EQ(drain::describe_duration(std::chrono::nanoseconds(1500)), std::string("1500ns"));
  DRAIN_CHECK_EQ(drain::describe_duration(std::chrono::milliseconds(2)),
                 std::string("2000000ns (2.000ms)"));
  DRAIN_CHECK_EQ(drain::describe_duration(std::chrono::nanoseconds(-2500)),
                 std::string("-2500ns"));
  DRAIN_CHECK_EQ(drain::describe_duration(std::chrono::nanoseconds(std::numeric_limits<std::int64_t>::min())),
                 std::string("-9223372036854775808ns (-9223372036854.775ms)"));
}

DRAIN_TEST(digest, known_vectors_and_stability) {
  DRAIN_CHECK_EQ(drain::crc32c("123456789", 9), 0xe3069283u);
  DRAIN_CHECK_EQ(drain::crc32c(""), 0u);
  DRAIN_CHECK_EQ(drain::hex_u64(0x0123456789abcdefULL), std::string("0123456789abcdef"));
  DRAIN_CHECK_EQ(drain::hex_u32(0xdeadbeefu), std::string("deadbeef"));
  drain::Digest64 first;
  drain::Digest64 second;
  first.update_tagged("a", "b");
  first.update_u64(7);
  second.update_tagged("a", "b");
  second.update_u64(7);
  DRAIN_CHECK_EQ(first.value(), second.value());
  second.update_bool(true);
  DRAIN_CHECK(first.value() != second.value());
}

// ---------------------------------------------------------------------------
// json
// ---------------------------------------------------------------------------

DRAIN_TEST(json, round_trip_preserves_order) {
  drain::JsonValue value = drain::JsonValue::object();
  value.set("z", drain::JsonValue(static_cast<std::int64_t>(1)));
  value.set("a", drain::JsonValue("two"));
  value.set("m", drain::JsonValue(true));
  const std::string text = value.dump();
  DRAIN_CHECK_EQ(text, std::string("{\"z\":1,\"a\":\"two\",\"m\":true}"));
  auto parsed = drain::json_parse(text);
  DRAIN_CHECK(parsed.ok());
  DRAIN_CHECK_EQ(parsed->dump(), text);
}

DRAIN_TEST(json, rejects_malformed_documents) {
  const char* bad[] = {"", "{", "}", "[1,]", "{\"a\":}", "{\"a\":1,}", "tru", "01", "1.",
                       "\"unterminated", "{\"a\":1,\"a\":2}", "nul", "1 2", "{\"a\" 1}",
                       "[1] junk", "\"\\u00\"", "\"\\ud800\"", "1e", "-"};
  for (const char* document : bad) {
    auto parsed = drain::json_parse(document);
    DRAIN_CHECK_MSG(!parsed.ok(), std::string("expected rejection of: ") + document);
  }
}

DRAIN_TEST(json, accepts_valid_edge_cases) {
  const char* good[] = {"0", "-0", "1e3", "\"\\u0041\\ud83d\\ude00\"", "[]", "{}",
                        "  {\"a\": [1, 2, {\"b\": null}]}  ", "18446744073709551615",
                        "-9223372036854775808"};
  for (const char* document : good) {
    auto parsed = drain::json_parse(document);
    DRAIN_CHECK_MSG(parsed.ok(), std::string("expected acceptance of: ") + document);
  }
  auto big = drain::json_parse("18446744073709551615");
  DRAIN_CHECK(big.ok() && big->is_uint());
}

DRAIN_TEST(json, enforces_limits) {
  drain::JsonLimits limits;
  limits.max_depth = 3;
  auto deep = drain::json_parse("[[[[1]]]]", limits);
  DRAIN_CHECK(!deep.ok());
  DRAIN_CHECK_EQ(deep.error().code(), drain::ErrorCode::BoundsExceeded);
  limits = drain::JsonLimits{};
  limits.max_total_bytes = 4;
  auto large = drain::json_parse("{\"key\": 1}", limits);
  DRAIN_CHECK(!large.ok());
  limits = drain::JsonLimits{};
  limits.max_nodes = 2;
  auto nodes = drain::json_parse("[1,2,3]", limits);
  DRAIN_CHECK(!nodes.ok());
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

DRAIN_TEST(lifecycle, legality_table) {
  DRAIN_CHECK(drain::is_legal_drain_transition(drain::DrainState::Requested, drain::DrainState::Validating));
  DRAIN_CHECK(drain::is_legal_drain_transition(drain::DrainState::Quiescing, drain::DrainState::Evacuating));
  DRAIN_CHECK(drain::is_legal_drain_transition(drain::DrainState::Verifying, drain::DrainState::Quiescing));
  DRAIN_CHECK(drain::is_legal_drain_transition(drain::DrainState::Drained, drain::DrainState::Restoring));
  DRAIN_CHECK(!drain::is_legal_drain_transition(drain::DrainState::Requested, drain::DrainState::Drained));
  DRAIN_CHECK(!drain::is_legal_drain_transition(drain::DrainState::Drained, drain::DrainState::Evacuating));
  DRAIN_CHECK(!drain::is_legal_drain_transition(drain::DrainState::Cancelled, drain::DrainState::Drained));
  DRAIN_CHECK(drain::is_settled(drain::DrainState::Drained));
  DRAIN_CHECK(!drain::is_settled(drain::DrainState::Restoring));
  DRAIN_CHECK(drain::is_active(drain::DrainState::Blocked));
}

DRAIN_TEST(lifecycle, record_refuses_completion_with_protected_work) {
  const auto target = target_of("r1", "d1");
  drain::DrainRecord record(drain::DrainId{1}, target, drain::Generation{1}, drain::AuthorityId{1},
                            drain::TimePoint{0});
  DRAIN_CHECK_CODE(record.record_completion(drain::TimePoint{1}, 3, 0xabc, drain::EvidenceSeq{1}),
                   drain::ErrorCode::ProtectedObligationRemains);
  DRAIN_CHECK(record.record_completion(drain::TimePoint{1}, 0, 0xabc, drain::EvidenceSeq{1}).ok());
  DRAIN_CHECK_EQ(record.completion_digest(), 0xabcULL);
}

DRAIN_TEST(lifecycle, blocked_remembers_where_it_came_from) {
  const auto target = target_of("r1", "d1");
  drain::DrainRecord record(drain::DrainId{1}, target, drain::Generation{1}, drain::AuthorityId{1},
                            drain::TimePoint{0});
  DRAIN_CHECK(record.transition(drain::DrainState::Validating, drain::TimePoint{1}, "v").ok());
  DRAIN_CHECK(record.transition(drain::DrainState::AdmissionClosed, drain::TimePoint{2}, "c").ok());
  DRAIN_CHECK(record.transition(drain::DrainState::Evacuating, drain::TimePoint{3}, "e").ok());
  DRAIN_CHECK(record.transition(drain::DrainState::Blocked, drain::TimePoint{4}, "b").ok());
  DRAIN_CHECK(record.blocked_from().has_value());
  DRAIN_CHECK_EQ(static_cast<int>(*record.blocked_from()), static_cast<int>(drain::DrainState::Evacuating));
  DRAIN_CHECK(record.transition(drain::DrainState::Evacuating, drain::TimePoint{5}, "r").ok());
  DRAIN_CHECK(!record.blocked_from().has_value());
  DRAIN_CHECK_CODE(record.transition(drain::DrainState::Requested, drain::TimePoint{6}, "x"),
                   drain::ErrorCode::IllegalTransition);
}

DRAIN_TEST(lifecycle, blockers_are_normalised) {
  drain::DrainRecord record;
  std::vector<drain::Blocker> blockers;
  drain::Blocker second;
  second.code = drain::BlockerCode::CapacityInsufficient;
  second.subject = "cp1";
  drain::Blocker first;
  first.code = drain::BlockerCode::ProtectedObligationActive;
  first.subject = "obligation:1";
  blockers.push_back(second);
  blockers.push_back(first);
  blockers.push_back(second);
  record.set_blockers(blockers);
  DRAIN_CHECK_EQ(record.blockers().size(), 2u);
  DRAIN_CHECK_EQ(static_cast<int>(record.blockers()[0].code),
                 static_cast<int>(drain::BlockerCode::ProtectedObligationActive));
  DRAIN_CHECK(record.has_blocker(drain::BlockerCode::CapacityInsufficient));
}

// ---------------------------------------------------------------------------
// obligations
// ---------------------------------------------------------------------------

DRAIN_TEST(obligation, dependencies_are_normalised_and_bounded) {
  std::vector<drain::DrainTarget> deps{target_of("b", "d1"), target_of("a", "d1"), target_of("b", "d1")};
  drain::Obligation obligation(drain::ObligationKind::ActiveFlow, holder_of("h1"), deps);
  DRAIN_CHECK_EQ(obligation.dependencies().size(), 2u);
  DRAIN_CHECK(obligation.depends_on(target_of("a", "d1")));
  DRAIN_CHECK(obligation.depends_on_resource(*drain::ResourceId::parse("b")));
  DRAIN_CHECK(!obligation.depends_on(target_of("c", "d1")));
  DRAIN_CHECK(obligation.blocks_completion());
  DRAIN_CHECK_EQ(static_cast<int>(obligation.evacuation_mode()),
                 static_cast<int>(drain::EvacuationMode::RerouteAndRelease));
}

DRAIN_TEST(obligation, state_machine_rejects_stale_revisions_and_appearance) {
  drain::Obligation obligation(drain::ObligationKind::ActiveFlow, holder_of("h1"), {target_of("a", "d1")});
  DRAIN_CHECK_CODE(obligation.apply_state(drain::Revision{9}, drain::ObligationState::Released,
                                          drain::EvidenceSeq{1}, drain::TimePoint{1}),
                   drain::ErrorCode::StaleRevision);
  DRAIN_CHECK(obligation.apply_state(obligation.revision(), drain::ObligationState::Released,
                                     drain::EvidenceSeq{4}, drain::TimePoint{5})
                  .ok());
  DRAIN_CHECK_EQ(obligation.release_evidence().value(), 4u);
  // Released is an assertion by the owner, not a proof: the obligation keeps
  // blocking completion until the engine retires it.
  DRAIN_CHECK(obligation.blocks_completion());
  DRAIN_CHECK(obligation.apply_state(obligation.revision(), drain::ObligationState::Retired,
                                     drain::EvidenceSeq{4}, drain::TimePoint{6})
                  .ok());
  DRAIN_CHECK_EQ(obligation.state(), drain::ObligationState::Retired);
  DRAIN_CHECK(!obligation.blocks_completion());
  // Reappearance: the obligation is live again and must block completion.
  DRAIN_CHECK(obligation.apply_state(obligation.revision(), drain::ObligationState::Admitted,
                                     drain::EvidenceSeq{}, drain::TimePoint{7})
                  .ok());
  DRAIN_CHECK(obligation.blocks_completion());
  DRAIN_CHECK_EQ(obligation.reappearances(), 1u);
  DRAIN_CHECK_EQ(obligation.release_evidence().value(), 0u);
}

DRAIN_TEST(obligation, table_assigns_monotonic_ids_and_never_reuses) {
  drain::ObligationTable table(4);
  for (int index = 0; index < 4; ++index) {
    drain::Obligation draft(drain::ObligationKind::Reservation, holder_of("h1"), {target_of("a", "d1")});
    auto inserted = table.insert(std::move(draft));
    DRAIN_CHECK(inserted.ok());
    DRAIN_CHECK_EQ(inserted->id().value(), static_cast<std::uint64_t>(index + 1));
  }
  drain::Obligation overflow(drain::ObligationKind::Reservation, holder_of("h1"), {target_of("a", "d1")});
  DRAIN_CHECK_CODE(table.insert(std::move(overflow)), drain::ErrorCode::BoundsExceeded);
  DRAIN_CHECK_EQ(table.size(), 4u);
  DRAIN_CHECK_EQ(table.high_water().value(), 4u);

  // Ids are never reused, including through the recovery path.
  drain::ObligationTable roomy(4);
  const drain::Obligation stored = *table.find(drain::ObligationId{1});
  DRAIN_CHECK(roomy.insert(stored).ok());
  DRAIN_CHECK_CODE(roomy.insert_with_id(stored), drain::ErrorCode::DuplicateIdentity);
  drain::Obligation second(drain::ObligationKind::Reservation, holder_of("h1"), {target_of("a", "d1")});
  auto second_stored = roomy.insert(std::move(second));
  DRAIN_CHECK(second_stored.ok());
  DRAIN_CHECK_EQ(second_stored->id().value(), 2u);
  DRAIN_CHECK_EQ(roomy.high_water().value(), 2u);
}

DRAIN_TEST(obligation, json_round_trip_preserves_state) {
  drain::ObligationTable table(8);
  drain::Obligation draft(drain::ObligationKind::CapacityGuarantee, holder_of("h2"),
                          {target_of("a", "d1"), target_of("b", "d2")});
  draft.set_protection(drain::ProtectionClass::Advisory);
  draft.set_capacity(drain::CapacityUnits::from(17));
  auto inserted = table.insert(std::move(draft));
  DRAIN_CHECK(inserted.ok());
  (void)table.find_mutable(inserted->id())
      ->apply_state(inserted->revision(), drain::ObligationState::Released, drain::EvidenceSeq{9},
                    drain::TimePoint{11});
  const drain::JsonValue encoded = table.to_json();
  drain::ObligationTable restored(8);
  DRAIN_CHECK_OK(restored.load_from_json(encoded));
  DRAIN_CHECK_EQ(restored.size(), 1u);
  const drain::Obligation* loaded = restored.find(drain::ObligationId{1});
  DRAIN_CHECK(loaded != nullptr);
  DRAIN_CHECK_EQ(static_cast<int>(loaded->state()), static_cast<int>(drain::ObligationState::Released));
  DRAIN_CHECK_EQ(loaded->capacity().value(), 17u);
  DRAIN_CHECK_EQ(loaded->dependencies().size(), 2u);
  DRAIN_CHECK_EQ(restored.digest(), table.digest());
}

// ---------------------------------------------------------------------------
// topology
// ---------------------------------------------------------------------------

namespace {

drain::Topology small_topology() {
  drain::Topology topology;
  const auto a = target_of("a", "d1");
  const auto b = target_of("b", "d1");
  const auto c = target_of("c", "d1");
  (void)topology.add_resource(drain::ResourceNode(a, drain::CapacityUnits::from(100)));
  (void)topology.add_resource(drain::ResourceNode(b, drain::CapacityUnits::from(100)));
  (void)topology.add_resource(drain::ResourceNode(c, drain::CapacityUnits::from(100)));
  (void)topology.add_edge(a.id(), b.id());
  (void)topology.add_edge(b.id(), c.id());
  drain::FabricPath direct;
  direct.id = *drain::ResourceId::parse("p1");
  direct.hops.push_back(a.id());
  (void)topology.add_path(direct);
  drain::FabricPath detour;
  detour.id = *drain::ResourceId::parse("p2");
  detour.hops.push_back(b.id());
  detour.hops.push_back(c.id());
  (void)topology.add_path(detour);
  drain::DiversityGroup group;
  group.id = *drain::GroupId::parse("dg");
  group.required_available = drain::MemberCount::from(1);
  group.paths = {direct.id, detour.id};
  (void)topology.add_diversity_group(group);
  return topology;
}

}  // namespace

DRAIN_TEST(topology, rejects_duplicate_and_invalid_entries) {
  drain::Topology topology = small_topology();
  DRAIN_CHECK_CODE(topology.add_resource(drain::ResourceNode(target_of("a", "d1"),
                                                             drain::CapacityUnits::from(1))),
                   drain::ErrorCode::AlreadyExists);
  DRAIN_CHECK_CODE(topology.add_edge(*drain::ResourceId::parse("a"), *drain::ResourceId::parse("a")),
                   drain::ErrorCode::InvalidArgument);
  DRAIN_CHECK_CODE(topology.add_edge(*drain::ResourceId::parse("a"), *drain::ResourceId::parse("zz")),
                   drain::ErrorCode::NotFound);
  drain::DiversityGroup bad;
  bad.id = *drain::GroupId::parse("bad");
  bad.required_available = drain::MemberCount::from(5);
  bad.paths = {*drain::ResourceId::parse("p1")};
  DRAIN_CHECK_CODE(topology.add_diversity_group(bad), drain::ErrorCode::InvalidArgument);
}

DRAIN_TEST(topology, removal_validation_reports_diversity_loss) {
  drain::Topology topology = small_topology();
  // With a commitment of one available path, either single removal is fine
  // because the other path survives.
  DRAIN_CHECK(topology.validate_removal(*drain::ResourceId::parse("a"), 1, 1).ok());
  DRAIN_CHECK(topology.validate_removal(*drain::ResourceId::parse("b"), 1, 1).ok());

  // Raise the commitment to two paths: the primary path carries only a, so
  // removing a leaves a single available path.
  drain::Topology committed_topology = small_topology();
  drain::DiversityGroup stricter;
  stricter.id = *drain::GroupId::parse("dg-strict");
  stricter.required_available = drain::MemberCount::from(2);
  stricter.paths = {*drain::ResourceId::parse("p1"), *drain::ResourceId::parse("p2")};
  DRAIN_CHECK_OK(committed_topology.add_diversity_group(stricter));
  const auto violation = committed_topology.validate_removal(*drain::ResourceId::parse("a"), 1, 1);
  DRAIN_CHECK(!violation.ok());
  DRAIN_CHECK_EQ(static_cast<int>(violation.kind),
                 static_cast<int>(drain::TopologyViolation::Kind::DiversityLost));
  // Groups are checked in identifier order, so the stricter group is the one
  // that reports the violation.
  DRAIN_CHECK_EQ(violation.subject, std::string("dg-strict"));

  // A commitment that only one path satisfies is severed by removing that path.
  drain::Topology strict;
  DRAIN_CHECK_OK(
      strict.add_resource(drain::ResourceNode(target_of("a", "d1"), drain::CapacityUnits::from(100))));
  DRAIN_CHECK_OK(
      strict.add_resource(drain::ResourceNode(target_of("b", "d1"), drain::CapacityUnits::from(100))));
  drain::FabricPath only;
  only.id = *drain::ResourceId::parse("p-only");
  only.hops.push_back(*drain::ResourceId::parse("a"));
  DRAIN_CHECK_OK(strict.add_path(only));
  drain::DiversityGroup committed;
  committed.id = *drain::GroupId::parse("strict");
  committed.required_available = drain::MemberCount::from(1);
  committed.paths = {only.id};
  DRAIN_CHECK_OK(strict.add_diversity_group(committed));
  const auto sole = strict.validate_removal(*drain::ResourceId::parse("a"), 1, 1);
  DRAIN_CHECK(!sole.ok());
  DRAIN_CHECK_EQ(static_cast<int>(sole.kind),
                 static_cast<int>(drain::TopologyViolation::Kind::DiversityLost));
}

DRAIN_TEST(topology, removal_validation_reports_capacity_exhaustion) {
  drain::Topology topology = small_topology();
  drain::CapacityPool pool;
  pool.id = *drain::GroupId::parse("cp");
  pool.required = drain::CapacityUnits::from(150);
  pool.members = {*drain::ResourceId::parse("a"), *drain::ResourceId::parse("b")};
  DRAIN_CHECK_OK(topology.add_capacity_pool(pool));
  // c is not a pool member, so removing it cannot exhaust the commitment.
  DRAIN_CHECK(topology.validate_removal(*drain::ResourceId::parse("c"), 1, 1).ok());
  const auto violation = topology.validate_removal(*drain::ResourceId::parse("a"), 1, 1);
  DRAIN_CHECK(!violation.ok());
  DRAIN_CHECK_EQ(static_cast<int>(violation.kind),
                 static_cast<int>(drain::TopologyViolation::Kind::CapacityPoolExhausted));
  DRAIN_CHECK_EQ(violation.subject, std::string("cp"));
}

DRAIN_TEST(topology, route_enumeration_is_deterministic_and_bounded) {
  drain::Topology topology = small_topology();
  std::set<drain::ResourceId> avoid;
  const auto routes = topology.enumerate_routes(*drain::ResourceId::parse("a"),
                                                *drain::ResourceId::parse("c"), avoid, 4, 8);
  DRAIN_CHECK_EQ(routes.size(), 1u);
  DRAIN_CHECK_EQ(routes[0].size(), 3u);
  avoid.insert(*drain::ResourceId::parse("b"));
  const auto severed = topology.enumerate_routes(*drain::ResourceId::parse("a"),
                                                 *drain::ResourceId::parse("c"), avoid, 4, 8);
  DRAIN_CHECK(severed.empty());
}

DRAIN_TEST(topology, json_round_trip) {
  drain::Topology topology = small_topology();
  drain::ProtectedRoute route;
  route.id = *drain::GroupId::parse("pr");
  route.origin = *drain::ResourceId::parse("a");
  route.terminus = *drain::ResourceId::parse("c");
  DRAIN_CHECK_OK(topology.add_protected_route(route));
  const drain::JsonValue encoded = topology.to_json();
  drain::Topology restored;
  DRAIN_CHECK_OK(restored.load_from_json(encoded));
  DRAIN_CHECK_EQ(restored.resource_count(), topology.resource_count());
  DRAIN_CHECK_EQ(restored.to_json().dump(), encoded.dump());
  // An edge that references a resource which is not present must be rejected
  // rather than silently dropped.
  auto dangling = encoded;
  dangling.set("resources", drain::JsonValue(drain::JsonValue::Array{}));
  drain::Topology empty;
  DRAIN_CHECK_CODE(empty.load_from_json(dangling), drain::ErrorCode::PersistenceCorrupt);

  auto mangled = encoded;
  mangled.set("edges", drain::JsonValue(drain::JsonValue::Array{}));
  mangled.set("paths", drain::JsonValue(drain::JsonValue::Array{}));
  mangled.set("diversity_groups", drain::JsonValue(drain::JsonValue::Array{}));
  mangled.set("capacity_pools", drain::JsonValue(drain::JsonValue::Array{}));
  mangled.set("protected_routes", drain::JsonValue(drain::JsonValue::Array{}));
  drain::Topology stripped;
  DRAIN_CHECK_OK(stripped.load_from_json(mangled));
  DRAIN_CHECK_EQ(stripped.resource_count(), topology.resource_count());
  DRAIN_CHECK_EQ(stripped.paths().size(), 0u);
}

// ---------------------------------------------------------------------------
// policy
// ---------------------------------------------------------------------------

DRAIN_TEST(policy, defaults_validate_and_round_trip) {
  const drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  DRAIN_CHECK_OK(policy.validate());
  const drain::JsonValue encoded = policy.to_json();
  auto restored = drain::DrainPolicy::from_json(encoded);
  DRAIN_CHECK(restored.ok());
  DRAIN_CHECK_EQ(restored->fingerprint(), policy.fingerprint());
  DRAIN_CHECK_EQ(restored->to_json().dump(), encoded.dump());
}

DRAIN_TEST(policy, rejects_out_of_range_values) {
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_concurrent_drains_per_domain(0);
  DRAIN_CHECK_CODE(policy.validate(), drain::ErrorCode::PolicyViolation);
  policy = drain::DrainPolicy::defaults();
  policy.set_max_targets_per_request(drain::kHardMaxTargetsPerRequest + 1);
  DRAIN_CHECK_CODE(policy.validate(), drain::ErrorCode::PolicyViolation);
  policy = drain::DrainPolicy::defaults();
  policy.set_evidence_freshness(std::chrono::seconds(0));
  DRAIN_CHECK_CODE(policy.validate(), drain::ErrorCode::PolicyViolation);
  policy = drain::DrainPolicy::defaults();
  policy.set_capacity_headroom(1, 0);
  DRAIN_CHECK_CODE(policy.validate(), drain::ErrorCode::PolicyViolation);
}

DRAIN_TEST(policy, per_kind_grace_overrides_default) {
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_default_grace(std::chrono::seconds(10));
  policy.set_grace_for_kind(drain::ObligationKind::Reservation, std::chrono::seconds(1));
  drain::Obligation reservation(drain::ObligationKind::Reservation, holder_of("h"), {target_of("a", "d1")});
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("h"), {target_of("a", "d1")});
  DRAIN_CHECK_EQ(policy.grace_for(reservation).count(), 1000000000);
  DRAIN_CHECK_EQ(policy.grace_for(flow).count(), 10000000000);
  reservation.set_grace(std::chrono::seconds(7));
  DRAIN_CHECK_EQ(policy.grace_for(reservation).count(), 7000000000);
}

// ---------------------------------------------------------------------------
// evidence ledger
// ---------------------------------------------------------------------------

namespace {

drain::EvidenceKey sample_key() {
  drain::EvidenceKey key;
  key.kind = drain::EvidenceKind::FlowQuiesced;
  key.drain = drain::DrainId{1};
  key.resource = *drain::ResourceId::parse("a");
  return key;
}

drain::Evidence sample_evidence(std::uint64_t sequence) {
  drain::Evidence evidence;
  evidence.seq = drain::EvidenceSeq{sequence};
  evidence.key = sample_key();
  evidence.generation = drain::Generation{1};
  evidence.epoch = drain::Epoch{1};
  evidence.producer = drain::IncarnationId{1};
  evidence.observed_at = drain::TimePoint{100};
  evidence.healthy = true;
  return evidence;
}

}  // namespace

DRAIN_TEST(evidence, monotonic_sequences_and_replay_rejection) {
  drain::EvidenceLedger ledger(8);
  const drain::EvidenceFence fence{drain::IncarnationId{1}, drain::Epoch{1}, drain::TimePoint{200}};
  DRAIN_CHECK(ledger.record(sample_evidence(1), fence, true).ok());
  DRAIN_CHECK_CODE(ledger.record(sample_evidence(1), fence, true), drain::ErrorCode::DuplicateIdentity);
  DRAIN_CHECK_CODE(ledger.record(sample_evidence(0), fence, true), drain::ErrorCode::DuplicateIdentity);
  DRAIN_CHECK(ledger.record(sample_evidence(2), fence, true).ok());
  DRAIN_CHECK_EQ(ledger.high_water(sample_key()).value(), 2u);
  DRAIN_CHECK_EQ(ledger.global_high_water().value(), 2u);
}

DRAIN_TEST(evidence, rejects_future_stale_epoch_and_foreign_incarnation) {
  drain::EvidenceLedger ledger(8);
  const drain::EvidenceFence fence{drain::IncarnationId{1}, drain::Epoch{4}, drain::TimePoint{100}};
  drain::Evidence future = sample_evidence(1);
  future.observed_at = drain::TimePoint{101};
  future.epoch = drain::Epoch{4};
  DRAIN_CHECK_CODE(ledger.record(future, fence, true), drain::ErrorCode::InvalidArgument);
  drain::Evidence stale_epoch = sample_evidence(1);
  stale_epoch.epoch = drain::Epoch{3};
  DRAIN_CHECK_CODE(ledger.record(stale_epoch, fence, true), drain::ErrorCode::StaleEpoch);
  drain::Evidence foreign = sample_evidence(1);
  foreign.epoch = drain::Epoch{4};
  foreign.producer = drain::IncarnationId{99};
  DRAIN_CHECK_CODE(ledger.record(foreign, fence, true), drain::ErrorCode::StaleIncarnation);
  DRAIN_CHECK(ledger.record(foreign, fence, false).ok());
}

DRAIN_TEST(evidence, freshness_window_is_enforced) {
  drain::EvidenceLedger ledger(8);
  drain::EvidenceFence fence{drain::IncarnationId{1}, drain::Epoch{1}, drain::TimePoint{100}};
  DRAIN_CHECK(ledger.record(sample_evidence(1), fence, true).ok());
  const drain::Evidence* stored = ledger.latest(sample_key());
  DRAIN_CHECK(stored != nullptr);
  DRAIN_CHECK(ledger.is_fresh(*stored, fence, std::chrono::seconds(1)));
  fence.now = drain::TimePoint{100 + 2000000000};
  DRAIN_CHECK(!ledger.is_fresh(*stored, fence, std::chrono::seconds(1)));
  DRAIN_CHECK(ledger.fresh(sample_key(), fence, std::chrono::seconds(1)) == nullptr);
  DRAIN_CHECK(ledger.fresh(sample_key(), fence, std::chrono::seconds(10)) != nullptr);
}

DRAIN_TEST(evidence, quarantine_blocks_reuse_after_recovery) {
  drain::EvidenceLedger ledger(8);
  const drain::EvidenceFence fence{drain::IncarnationId{1}, drain::Epoch{1}, drain::TimePoint{100}};
  DRAIN_CHECK(ledger.record(sample_evidence(1), fence, true).ok());
  ledger.quarantine_all();
  const drain::Evidence* stored = ledger.latest(sample_key());
  DRAIN_CHECK(stored != nullptr);
  DRAIN_CHECK(!stored->attested);
  DRAIN_CHECK(!ledger.is_fresh(*stored, fence, std::chrono::hours(1)));
  // The live incarnation may re-attest its own record; quarantine only stops a
  // recovered record from being used without a fresh attestation.
  DRAIN_CHECK_OK(ledger.attest(sample_key(), drain::EvidenceSeq{1}, fence));
  const drain::Evidence* reattested = ledger.latest(sample_key());
  DRAIN_CHECK(reattested != nullptr);
  DRAIN_CHECK(reattested->attested);
  DRAIN_CHECK(ledger.is_fresh(*reattested, fence, std::chrono::hours(1)));
}

DRAIN_TEST(evidence, re_attestation_requires_the_live_incarnation) {
  drain::EvidenceLedger ledger(8);
  const drain::EvidenceFence writer{drain::IncarnationId{1}, drain::Epoch{1}, drain::TimePoint{100}};
  DRAIN_CHECK(ledger.record(sample_evidence(1), writer, true).ok());
  ledger.quarantine_all();
  const drain::EvidenceFence successor{drain::IncarnationId{2}, drain::Epoch{2}, drain::TimePoint{150}};
  DRAIN_CHECK_CODE(ledger.attest(sample_key(), drain::EvidenceSeq{1}, successor),
                   drain::ErrorCode::StaleEpoch);
  DRAIN_CHECK_CODE(ledger.attest(sample_key(), drain::EvidenceSeq{99}, successor),
                   drain::ErrorCode::NotFound);
}

DRAIN_TEST(evidence, sequences_are_scoped_to_their_stream) {
  // Two independent streams may use the same sequence number. Keying records by
  // sequence alone silently aliased them, so a removal observation could be
  // read back as an unrelated release observation.
  drain::EvidenceLedger ledger(16);
  const drain::EvidenceFence fence{drain::IncarnationId{1}, drain::Epoch{1}, drain::TimePoint{500}};

  drain::Evidence release = sample_evidence(1);
  release.observed_at = drain::TimePoint{100};
  DRAIN_CHECK(ledger.record(release, fence, true).ok());

  drain::EvidenceKey removal;
  removal.kind = drain::EvidenceKind::ResourceRemoved;
  removal.drain = drain::DrainId{1};
  removal.resource = *drain::ResourceId::parse("a");
  drain::Evidence observation = sample_evidence(1);
  observation.key = removal;
  observation.observed_at = drain::TimePoint{400};
  DRAIN_CHECK(ledger.record(observation, fence, true).ok());

  DRAIN_CHECK_EQ(ledger.size(), 2u);
  const drain::Evidence* stored_release = ledger.find(sample_key(), drain::EvidenceSeq{1});
  const drain::Evidence* stored_removal = ledger.find(removal, drain::EvidenceSeq{1});
  DRAIN_CHECK(stored_release != nullptr);
  DRAIN_CHECK(stored_removal != nullptr);
  DRAIN_CHECK_EQ(stored_release->observed_at.count(), 100);
  DRAIN_CHECK_EQ(stored_removal->observed_at.count(), 400);
  DRAIN_CHECK_EQ(static_cast<int>(ledger.latest(removal)->key.kind),
                 static_cast<int>(drain::EvidenceKind::ResourceRemoved));

  // Re-recording the same (stream, sequence) pair is a duplicate, not an
  // overwrite.
  DRAIN_CHECK_CODE(ledger.record(observation, fence, true), drain::ErrorCode::DuplicateIdentity);

  // The pair survives a round trip.
  const drain::JsonValue encoded = ledger.to_json();
  drain::EvidenceLedger restored(16);
  DRAIN_CHECK_OK(restored.load_from_json(encoded));
  DRAIN_CHECK_EQ(restored.size(), 2u);
  const drain::Evidence* restored_removal = restored.find(removal, drain::EvidenceSeq{1});
  DRAIN_CHECK(restored_removal != nullptr);
  DRAIN_CHECK_EQ(restored_removal->observed_at.count(), 400);
}

DRAIN_TEST(evidence, ledger_bounds_growth_by_retiring_superseded_records) {
  drain::EvidenceLedger ledger(3);
  const drain::EvidenceFence fence{drain::IncarnationId{1}, drain::Epoch{1}, drain::TimePoint{1000}};
  for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
    drain::Evidence evidence = sample_evidence(sequence);
    evidence.observed_at = drain::TimePoint{100};
    DRAIN_CHECK(ledger.record(evidence, fence, true).ok());
  }
  DRAIN_CHECK_EQ(ledger.size(), ledger.max_records());
  // With distinct keys there is nothing superseded to retire, so the bound is a
  // hard rejection rather than silent growth.
  drain::EvidenceLedger narrow(2);
  for (std::uint64_t index = 0; index < 2; ++index) {
    drain::EvidenceKey key = sample_key();
    key.obligation = drain::ObligationId{index + 1};
    drain::Evidence evidence = sample_evidence(index + 1);
    evidence.key = key;
    DRAIN_CHECK(narrow.record(evidence, fence, true).ok());
  }
  DRAIN_CHECK_EQ(narrow.size(), 2u);
  drain::EvidenceKey extra = sample_key();
  extra.obligation = drain::ObligationId{99};
  drain::Evidence beyond = sample_evidence(99);
  beyond.key = extra;
  DRAIN_CHECK_CODE(narrow.record(beyond, fence, true), drain::ErrorCode::BoundsExceeded);
}

// ---------------------------------------------------------------------------
// authority
// ---------------------------------------------------------------------------

DRAIN_TEST(authority, generation_epoch_and_incarnation_fences) {
  drain::AuthorityRegistry registry;
  DRAIN_CHECK_CODE(registry.install_incarnation(drain::IncarnationId{0}, drain::TimePoint{0}),
                   drain::ErrorCode::InvalidArgument);
  DRAIN_CHECK_OK(registry.install_incarnation(drain::IncarnationId{1}, drain::TimePoint{0}));
  DRAIN_CHECK_CODE(registry.install_incarnation(drain::IncarnationId{1}, drain::TimePoint{1}),
                   drain::ErrorCode::StaleIncarnation);
  const drain::TimePoint issued_at{0};
  auto token = registry.issue(*drain::DomainId::parse("d1"), issued_at, std::chrono::seconds(10));
  DRAIN_CHECK(token.ok());
  DRAIN_CHECK_OK(registry.validate(*token, issued_at + std::chrono::seconds(5)));
  DRAIN_CHECK_CODE(registry.validate(*token, issued_at + std::chrono::seconds(11)),
                   drain::ErrorCode::StaleAuthority);
  DRAIN_CHECK_OK(registry.advance_scope_generation(*drain::DomainId::parse("d1")));
  DRAIN_CHECK_CODE(registry.validate(*token, issued_at + std::chrono::seconds(5)),
                   drain::ErrorCode::StaleGeneration);
  DRAIN_CHECK_OK(registry.begin_new_epoch(drain::IncarnationId{2}, issued_at + std::chrono::seconds(20),
                                          "restart"));
  DRAIN_CHECK_CODE(registry.validate(*token, issued_at + std::chrono::seconds(21)),
                   drain::ErrorCode::StaleAuthority);
  DRAIN_CHECK_EQ(registry.epoch().value(), 2u);
}

DRAIN_TEST(authority, tokens_load_as_revoked) {
  drain::AuthorityRegistry registry;
  DRAIN_CHECK_OK(registry.install_incarnation(drain::IncarnationId{3}, drain::TimePoint{0}));
  auto token = registry.issue(drain::DomainId{}, drain::TimePoint{0}, std::chrono::hours(1));
  DRAIN_CHECK(token.ok());
  const drain::JsonValue encoded = registry.to_json();
  drain::AuthorityRegistry restored;
  DRAIN_CHECK_OK(restored.load_from_json(encoded));
  const drain::AuthorityToken* loaded = restored.find(token->id());
  DRAIN_CHECK(loaded != nullptr);
  DRAIN_CHECK(loaded->revoked());
  DRAIN_CHECK_CODE(restored.validate(*loaded, drain::TimePoint{1}), drain::ErrorCode::StaleAuthority);
}

// ---------------------------------------------------------------------------
// admission fence
// ---------------------------------------------------------------------------

DRAIN_TEST(admission, closure_and_reopen_advance_generation) {
  drain::AdmissionFence fence;
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(fence.check_admission(target, drain::Generation{}));
  auto first = fence.close(target, drain::DrainId{1}, drain::TimePoint{1}, "maintenance");
  DRAIN_CHECK(first.ok());
  DRAIN_CHECK_EQ(first->value(), 1u);
  DRAIN_CHECK(fence.is_closed(target));
  DRAIN_CHECK_CODE(fence.check_admission(target, *first), drain::ErrorCode::AdmissionClosed);
  DRAIN_CHECK_CODE(fence.check_admission(target, drain::Generation{}), drain::ErrorCode::StaleGeneration);
  auto reopened = fence.reopen(target, drain::DrainId{1}, drain::TimePoint{2}, "cancelled");
  DRAIN_CHECK(reopened.ok());
  DRAIN_CHECK_EQ(reopened->value(), 2u);
  DRAIN_CHECK(!fence.is_closed(target));
  DRAIN_CHECK_OK(fence.check_admission(target, *reopened));
  DRAIN_CHECK_CODE(fence.check_admission(target, *first), drain::ErrorCode::StaleGeneration);
  DRAIN_CHECK_CODE(fence.reopen(target, drain::DrainId{1}, drain::TimePoint{3}, "again"),
                   drain::ErrorCode::IllegalTransition);
}

DRAIN_TEST(admission, never_fenced_target_rejects_nonzero_generation) {
  drain::AdmissionFence fence;
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_CODE(fence.check_admission(target, drain::Generation{3}), drain::ErrorCode::StaleGeneration);
  DRAIN_CHECK_OK(fence.check_admission(target, drain::Generation{}));
}

// ---------------------------------------------------------------------------
// decisions and explanations
// ---------------------------------------------------------------------------

DRAIN_TEST(decision, rendering_is_stable) {
  drain::Decision decision;
  decision.action = "enter-evacuating";
  decision.outcome = drain::DecisionOutcome::Applied;
  decision.drain = drain::DrainId{4};
  decision.generation = drain::Generation{2};
  decision.epoch = drain::Epoch{1};
  decision.policy_revision = drain::Revision{1};
  decision.policy_fingerprint = "0123456789abcdef";
  decision.at = drain::TimePoint{1234};
  decision.inputs = {"outstanding-protected=2"};
  decision.rejected = {"declare-drained-early"};
  const std::string first = decision.render();
  const std::string second = decision.render();
  DRAIN_CHECK_EQ(first, second);
  DRAIN_CHECK(first.find("enter-evacuating") != std::string::npos);
  auto parsed = drain::Decision::from_json(decision.to_json());
  DRAIN_CHECK(parsed.ok());
  DRAIN_CHECK_EQ(parsed->render(), first);
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

DRAIN_TEST(persistence, round_trip_and_integrity_envelope) {
  const std::string directory = make_temp_directory("unit-persistence");
  const std::string path = directory + "/state.drainlog";
  drain::Persistence persistence(path);
  drain::JsonValue snapshot = drain::JsonValue::object();
  snapshot.set("value", drain::JsonValue(static_cast<std::int64_t>(42)));
  drain::SystemClock clock;
  DRAIN_CHECK_OK(persistence.save(snapshot, drain::IncarnationId{7}, clock.now()));
  DRAIN_CHECK(persistence.exists());

  drain::SnapshotInfo info;
  auto loaded = persistence.load(info);
  DRAIN_CHECK(loaded.ok());
  DRAIN_CHECK_EQ(loaded->get_uint("value"), 42u);
  DRAIN_CHECK_EQ(info.incarnation.value(), 7u);
  DRAIN_CHECK_EQ(info.sequence, 1u);
  DRAIN_CHECK(!info.from_backup);

  DRAIN_CHECK_OK(persistence.save(snapshot, drain::IncarnationId{8}, clock.now()));
  DRAIN_CHECK_OK(persistence.verify(info));
  DRAIN_CHECK_EQ(info.sequence, 2u);
  DRAIN_CHECK_OK(persistence.reset());
  DRAIN_CHECK(!persistence.exists());
}

DRAIN_TEST(persistence, rejects_oversized_payloads) {
  const std::string directory = make_temp_directory("unit-persistence-oversize");
  const std::string path = directory + "/state.drainlog";
  drain::PersistenceLimits limits;
  limits.max_snapshot_bytes = 64;
  drain::Persistence persistence(path, limits);
  drain::JsonValue snapshot = drain::JsonValue::object();
  snapshot.set("value", drain::JsonValue(std::string(200, 'x')));
  drain::SystemClock clock;
  DRAIN_CHECK_CODE(persistence.save(snapshot, drain::IncarnationId{1}, clock.now()),
                   drain::ErrorCode::BoundsExceeded);
  DRAIN_CHECK(!persistence.exists());
}

// ---------------------------------------------------------------------------
// engine basics
// ---------------------------------------------------------------------------

DRAIN_TEST(engine, requires_authority_for_every_mutation) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  auto engine = make_engine(policy, clock);
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(
      engine->add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)), clock.now()));
  drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("h"), {target});
  DRAIN_CHECK_CODE(engine->admit_obligation(draft, drain::Generation{}, drain::AuthorityId{}, clock.now()),
                   drain::ErrorCode::AuthorityRequired);
  drain::DrainRequest request;
  request.targets = {target};
  request.authority = drain::AuthorityId{};
  DRAIN_CHECK_CODE(engine->request_drain_set(request, clock.now()), drain::ErrorCode::AuthorityRequired);
}

DRAIN_TEST(engine, full_lifecycle_reaches_drained_and_closes_accounting) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_obligations(64);
  drain::DrainEngine engine(policy, clock);
  ScriptedSink sink(engine);
  engine.set_sink(&sink);
  const drain::TimePoint start = clock.now();
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, start));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  DRAIN_CHECK(authority.ok());

  const TopologyShape shape;
  const auto targets = build_topology(engine, shape, start);
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("workload"), {targets[0], targets[1]});
  DRAIN_CHECK(engine.admit_obligation(flow, drain::Generation{}, authority->id(), clock.now()).ok());

  drain::DrainRequest request;
  request.targets = {targets[0]};
  request.reason = "unit test";
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(set.ok());
  const auto stored = engine.drain_set(*set);
  const drain::DrainId id = stored->members.front();

  for (int step = 0; step < 64; ++step) {
    clock.advance(std::chrono::seconds(1));
    DRAIN_CHECK_OK(engine.advance(clock.now()));
    const auto record = engine.drain(id);
    if (record.has_value() && drain::is_settled(record->state())) {
      break;
    }
  }
  const auto record = engine.drain(id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Drained));
  DRAIN_CHECK_EQ(record->remaining_protected_at_completion(), 0u);
  DRAIN_CHECK(record->completion_digest() != 0);
  DRAIN_CHECK_EQ(sink.calls_under_lock, 0u);

  const drain::AccountingReport audit = engine.audit(clock.now());
  DRAIN_CHECK_MSG(audit.clean(), audit.render());
  DRAIN_CHECK_EQ(audit.drains_drained, 1u);
  DRAIN_CHECK_EQ(audit.obligations_outstanding_protected, 0u);
  DRAIN_CHECK_EQ(engine.outstanding_protected_count(targets[0]), 0u);
}

DRAIN_TEST(engine, admission_after_closure_is_refused) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  drain::DrainEngine engine(policy, clock);
  ScriptedSink sink(engine);
  sink.mode = ScriptedSink::Mode::RecordOnly;
  engine.set_sink(&sink);
  const drain::TimePoint start = clock.now();
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, start));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)), start));

  drain::DrainRequest request;
  request.targets = {target};
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(set.ok());
  const auto stored = engine.drain_set(*set);
  const drain::DrainId id = stored->members.front();

  (void)engine.advance(clock.now());
  (void)engine.advance(clock.now());
  const auto record = engine.drain(id);
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::AdmissionClosed));
  const drain::Generation closed = record->generation();

  drain::Obligation late(drain::ObligationKind::ActiveFlow, holder_of("late"), {target});
  DRAIN_CHECK_CODE(engine.admit_obligation(late, closed, authority->id(), clock.now()),
                   drain::ErrorCode::AdmissionClosed);
  DRAIN_CHECK_CODE(engine.admit_obligation(late, drain::Generation{}, authority->id(), clock.now()),
                   drain::ErrorCode::StaleGeneration);
  DRAIN_CHECK_EQ(engine.audit(clock.now()).violations.size(), 0u);
}

DRAIN_TEST(engine, explanations_are_reproducible) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_evacuation_attempts(1);
  policy.set_evacuation_retry_interval(std::chrono::seconds(0));
  drain::DrainEngine engine(policy, clock);
  ScriptedSink sink(engine);
  sink.mode = ScriptedSink::Mode::RecordOnly;
  engine.set_sink(&sink);
  const drain::TimePoint start = clock.now();
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, start));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)), start));
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("h"), {target});
  DRAIN_CHECK(engine.admit_obligation(flow, drain::Generation{}, authority->id(), clock.now()).ok());

  drain::DrainRequest request;
  request.targets = {target};
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  const auto stored = engine.drain_set(*set);
  const drain::DrainId id = stored->members.front();
  for (int step = 0; step < 6; ++step) {
    clock.advance(std::chrono::seconds(1));
    (void)engine.advance(clock.now());
  }
  const drain::Explanation first = engine.explain(id);
  const drain::Explanation second = engine.explain(id);
  DRAIN_CHECK_EQ(first.render(), second.render());
  DRAIN_CHECK_EQ(first.to_json().dump(), second.to_json().dump());
  DRAIN_CHECK(first.render().find("blockers:") != std::string::npos);
  DRAIN_CHECK(first.blockers.size() > 0);
}

DRAIN_TEST(engine, duplicate_nonce_returns_the_original_set) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  drain::DrainEngine engine(policy, clock);
  const drain::TimePoint start = clock.now();
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, start));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)), start));
  drain::DrainRequest request;
  request.targets = {target};
  request.authority = authority->id();
  request.nonce = drain::RequestNonce{77};
  auto first = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(first.ok());
  auto second = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(second.ok());
  DRAIN_CHECK_EQ(first->value(), second->value());
  DRAIN_CHECK_EQ(engine.drains().size(), 1u);

  drain::DrainRequest conflicting = request;
  conflicting.targets = {target_of("r2", "d1")};
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(conflicting.targets[0],
                                                         drain::CapacityUnits::from(10)), start));
  DRAIN_CHECK_CODE(engine.request_drain_set(conflicting, clock.now()),
                   drain::ErrorCode::DuplicateIdentity);
}

DRAIN_TEST(engine, correlated_set_rejects_unsatisfiable_commitments) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_concurrent_drains_per_domain(4);
  drain::DrainEngine engine(policy, clock);
  const drain::TimePoint start = clock.now();
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, start));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  const TopologyShape shape;
  const auto targets = build_topology(engine, shape, start);

  drain::DrainRequest request;
  request.targets = {targets[0], targets[1], targets[2]};
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK_CODE(set, drain::ErrorCode::CapacityViolation);
  DRAIN_CHECK(engine.drains().empty());

  drain::DrainRequest pair;
  pair.targets = {targets[0], targets[1]};
  pair.authority = authority->id();
  DRAIN_CHECK_CODE(engine.request_drain_set(pair, clock.now()), drain::ErrorCode::RedundancyViolation);
  DRAIN_CHECK(engine.drains().empty());
}

DRAIN_TEST(engine, per_domain_concurrency_limit_is_enforced) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_concurrent_drains_per_domain(1);
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  drain::DrainEngine engine(policy, clock);
  ScriptedSink sink(engine);
  sink.mode = ScriptedSink::Mode::RecordOnly;
  engine.set_sink(&sink);
  const drain::TimePoint start = clock.now();
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, start));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  drain::DrainRequest request;
  request.targets = {target_of("a", "d1")};
  request.authority = authority->id();
  request.nonce = drain::RequestNonce{1};
  (void)engine.add_resource(drain::ResourceNode(request.targets[0], drain::CapacityUnits::from(10)), start);
  auto first = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(first.ok());
  request.targets = {target_of("b", "d1")};
  request.nonce = drain::RequestNonce{2};
  (void)engine.add_resource(drain::ResourceNode(request.targets[0], drain::CapacityUnits::from(10)), start);
  auto second = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(second.ok());

  for (int step = 0; step < 3; ++step) {
    (void)engine.advance(clock.now());
  }
  const auto first_record = engine.drain(engine.drain_set(*first)->members.front());
  const auto second_record = engine.drain(engine.drain_set(*second)->members.front());
  // The first drain holds the domain slot; the second must not have closed
  // admission and must carry a deterministic explanation.
  DRAIN_CHECK(first_record->state() == drain::DrainState::AdmissionClosed ||
              first_record->state() == drain::DrainState::Quiescing);
  DRAIN_CHECK(second_record->state() != drain::DrainState::AdmissionClosed);
  DRAIN_CHECK(second_record->has_blocker(drain::BlockerCode::DomainLimitReached) ||
              second_record->has_blocker(drain::BlockerCode::SetPredecessorPending));
  DRAIN_CHECK_EQ(second_record->generation().value(), 0u);
}

DRAIN_TEST(engine, statistics_count_only_when_the_sink_was_actually_used) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  drain::DrainEngine engine(policy, clock);
  ScriptedSink sink(engine);
  sink.fail_requests = true;
  engine.set_sink(&sink);
  const drain::TimePoint start = clock.now();
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, start));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), start);
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)), start));
  drain::Obligation flow(drain::ObligationKind::ActiveFlow, holder_of("h"), {target});
  DRAIN_CHECK(engine.admit_obligation(flow, drain::Generation{}, authority->id(), clock.now()).ok());
  drain::DrainRequest request;
  request.targets = {target};
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  const auto stored = engine.drain_set(*set);
  const drain::DrainId id = stored->members.front();
  for (int step = 0; step < 24; ++step) {
    clock.advance(std::chrono::seconds(10));
    (void)engine.advance(clock.now());
  }
  const auto record = engine.drain(id);
  DRAIN_CHECK(record->has_blocker(drain::BlockerCode::EvacuationAttemptsExhausted));
  DRAIN_CHECK(engine.stats().sink_failures == record->evacuation_requests());
  DRAIN_CHECK_EQ(record->evacuation_requests(), policy.max_evacuation_attempts());
}
