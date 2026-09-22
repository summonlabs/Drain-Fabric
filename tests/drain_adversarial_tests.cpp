// Drain Fabric -- adversarial input tests.
//
// Every externally derived byte string is treated as hostile: snapshot files,
// wire frames, policy and topology documents, JSON payloads, evidence reports,
// and CLI-style arguments. The tests below corrupt, truncate, reorder, duplicate,
// and overflow those inputs and require a clean, typed rejection.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "drain/engine.hpp"
#include "drain/persistence.hpp"
#include "drain/runtime.hpp"
#include "drain/policy.hpp"
#include "drain/topology.hpp"
#include "drain/wire.hpp"
#include "fixture.hpp"
#include "process.hpp"
#include "test.hpp"

using namespace drain_test;

namespace {

std::vector<unsigned char> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<unsigned char> bytes;
  if (!stream) {
    return bytes;
  }
  stream.seekg(0, std::ios::end);
  bytes.resize(static_cast<std::size_t>(stream.tellg()));
  stream.seekg(0, std::ios::beg);
  stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return bytes;
}

void write_bytes(const std::string& path, const std::vector<unsigned char>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
}

std::string corrupt_snapshot(std::uint64_t seed, const std::string& directory) {
  const std::string path = directory + "/corrupt-" + std::to_string(seed) + ".drainlog";
  drain::Persistence persistence(path);
  drain::JsonValue snapshot = drain::JsonValue::object();
  snapshot.set("value", drain::JsonValue(1234));
  snapshot.set("text", drain::JsonValue(std::string(64, 'a')));
  drain::SystemClock clock;
  DRAIN_CHECK_OK(persistence.save(snapshot, drain::IncarnationId{1}, clock.now()));
  std::vector<unsigned char> bytes = read_bytes(path);
  DRAIN_CHECK(bytes.size() > drain::kSnapshotOverheadBytes);

  Rng rng(seed);
  switch (seed % 6) {
    case 0: {
      const std::size_t index = 8 + rng.index(bytes.size() - 8);
      bytes[index] ^= static_cast<unsigned char>(1u << rng.index(8));
      break;
    }
    case 1:
      bytes.resize(bytes.size() - 1 - rng.index(8));
      break;
    case 2:
      bytes.resize(bytes.size() / 2);
      break;
    case 3:
      bytes[0] = 'X';
      break;
    case 4:
      bytes.insert(bytes.end(), 32, 0x7f);
      break;
    default: {
      const std::size_t index = 32 + rng.index(8);
      bytes[index] = 0xff;
      bytes[index + 1] = 0xff;
      bytes[index + 2] = 0xff;
      break;
    }
  }
  write_bytes(path, bytes);
  return path;
}

}  // namespace

DRAIN_TEST(adversarial, corrupted_snapshots_are_never_applied) {
  const std::string directory = make_temp_directory("adversarial-snapshot");
  for (std::uint64_t seed = 0; seed < 24; ++seed) {
    const std::string path = corrupt_snapshot(seed, directory);
    drain::Persistence persistence(path);
    drain::SnapshotInfo info;
    auto loaded = persistence.load(info);
    DRAIN_CHECK_MSG(!loaded.ok(), "a corrupted snapshot was accepted for seed " + std::to_string(seed));
    DRAIN_CHECK(loaded.error().code() == drain::ErrorCode::PersistenceCorrupt ||
                loaded.error().code() == drain::ErrorCode::BoundsExceeded ||
                loaded.error().code() == drain::ErrorCode::PersistenceIo);
    drain::Status verified = persistence.verify(info);
    DRAIN_CHECK(!verified.ok());
    remove_file(path);
  }
}

DRAIN_TEST(adversarial, truncated_snapshot_falls_back_to_the_previous_good_copy) {
  const std::string directory = make_temp_directory("adversarial-backup");
  const std::string path = directory + "/state.drainlog";
  drain::Persistence persistence(path);
  drain::SystemClock clock;
  drain::JsonValue first = drain::JsonValue::object();
  first.set("generation", drain::JsonValue(1));
  DRAIN_CHECK_OK(persistence.save(first, drain::IncarnationId{1}, clock.now()));
  drain::JsonValue second = drain::JsonValue::object();
  second.set("generation", drain::JsonValue(2));
  DRAIN_CHECK_OK(persistence.save(second, drain::IncarnationId{2}, clock.now()));

  std::vector<unsigned char> bytes = read_bytes(path);
  bytes.resize(bytes.size() / 3);
  write_bytes(path, bytes);

  drain::SnapshotInfo info;
  auto recovered = persistence.load(info);
  DRAIN_CHECK_MSG(recovered.ok(), recovered.ok() ? "" : recovered.error().message());
  DRAIN_CHECK_EQ(recovered->get_uint("generation"), 1u);
  DRAIN_CHECK(info.from_backup);
  DRAIN_CHECK_EQ(info.incarnation.value(), 1u);
}

DRAIN_TEST(adversarial, empty_and_missing_snapshots_report_cleanly) {
  const std::string directory = make_temp_directory("adversarial-empty");
  const std::string path = directory + "/missing.drainlog";
  remove_file(path);
  remove_file(path + ".bak");
  drain::Persistence persistence(path);
  drain::SnapshotInfo info;
  DRAIN_CHECK_CODE(persistence.load(info), drain::ErrorCode::NotFound);
  write_bytes(path, {});
  DRAIN_CHECK_CODE(persistence.load(info), drain::ErrorCode::PersistenceCorrupt);
  const std::vector<unsigned char> tiny(drain::kSnapshotOverheadBytes - 1, 0);
  write_bytes(path, tiny);
  DRAIN_CHECK_CODE(persistence.load(info), drain::ErrorCode::PersistenceCorrupt);
}

DRAIN_TEST(adversarial, frame_decoder_rejects_every_malformed_image) {
  drain::Frame frame;
  frame.type = drain::MessageType::DrainRequest;
  frame.sequence = 7;
  drain::JsonValue payload = drain::JsonValue::object();
  payload.set("targets", drain::JsonValue(drain::JsonValue::Array{}));
  frame.payload = payload;
  auto encoded = drain::encode_frame(frame, 4096);
  DRAIN_CHECK(encoded.ok());

  DRAIN_CHECK(!drain::decode_frame(encoded->data(), encoded->size() - 1, 4096).ok());
  DRAIN_CHECK(!drain::decode_frame(encoded->data(), drain::kFrameHeaderBytes - 1, 4096).ok());
  DRAIN_CHECK(!drain::decode_frame(encoded->data(), encoded->size(), 4).ok());

  for (std::size_t index = 0; index < encoded->size(); ++index) {
    std::vector<unsigned char> mutated = *encoded;
    mutated[index] ^= 0xff;
    auto decoded = drain::decode_frame(mutated.data(), mutated.size(), 4096);
    DRAIN_CHECK_MSG(!decoded.ok(), "mutation at byte " + std::to_string(index) + " was accepted");
  }

  std::vector<unsigned char> appended = *encoded;
  appended.push_back(0);
  DRAIN_CHECK(!drain::decode_frame(appended.data(), appended.size(), 4096).ok());

  drain::Frame oversized;
  oversized.type = drain::MessageType::DrainRequest;
  oversized.payload = drain::JsonValue(std::string(256, 'z'));
  DRAIN_CHECK(!drain::encode_frame(oversized, 32).ok());
}

DRAIN_TEST(adversarial, json_parser_survives_random_bytes) {
  Rng rng(4242);
  const char alphabet[] = "{}[]\":,\\0123456789.eE+-tfnul \t\n\r\x01\x7f";
  const std::size_t alphabet_size = sizeof(alphabet) - 1;
  for (std::size_t iteration = 0; iteration < 4000; ++iteration) {
    const std::size_t length = rng.index(48);
    std::string document;
    document.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      document.push_back(alphabet[rng.index(alphabet_size)]);
    }
    auto parsed = drain::json_parse(document);
    if (parsed.ok()) {
      // Anything the parser accepts must round trip through the writer.
      const std::string dumped = parsed->dump();
      auto again = drain::json_parse(dumped);
      DRAIN_CHECK_MSG(again.ok(), "writer produced unparsable output for: " + document);
    }
  }
}

DRAIN_TEST(adversarial, policy_document_bounds_are_enforced) {
  drain::JsonValue document = drain::DrainPolicy::defaults().to_json();
  document.set("max_obligations", drain::JsonValue(static_cast<std::uint64_t>(1) << 40));
  auto rejected = drain::DrainPolicy::from_json(document);
  DRAIN_CHECK(!rejected.ok());

  document = drain::DrainPolicy::defaults().to_json();
  document.set("revision", drain::JsonValue(static_cast<std::uint64_t>(0)));
  DRAIN_CHECK(!drain::DrainPolicy::from_json(document).ok());

  document = drain::DrainPolicy::defaults().to_json();
  document.set("name", drain::JsonValue(std::string(400, 'n')));
  DRAIN_CHECK(!drain::DrainPolicy::from_json(document).ok());

  document = drain::DrainPolicy::defaults().to_json();
  document.set("evidence_freshness_ns", drain::JsonValue(-5));
  DRAIN_CHECK(!drain::DrainPolicy::from_json(document).ok());
}

DRAIN_TEST(adversarial, topology_document_rejects_duplicates_and_bad_shapes) {
  drain::Topology topology;
  const auto a = target_of("a", "d1");
  DRAIN_CHECK_OK(topology.add_resource(drain::ResourceNode(a, drain::CapacityUnits::from(10))));
  drain::JsonValue document = topology.to_json();
  document.find("resources")->as_array().push_back(drain::to_json(a));
  drain::Topology loaded;
  DRAIN_CHECK_CODE(loaded.load_from_json(document), drain::ErrorCode::PersistenceCorrupt);

  document = topology.to_json();
  document.set("resources", drain::JsonValue("not an array"));
  DRAIN_CHECK_CODE(loaded.load_from_json(document), drain::ErrorCode::PersistenceCorrupt);

  document = topology.to_json();
  document.set("resources", drain::JsonValue(drain::JsonValue::Array{}));
  DRAIN_CHECK_OK(loaded.load_from_json(document));
  DRAIN_CHECK_EQ(loaded.resource_count(), 0u);

  drain::ResourceNode node = drain::ResourceNode(a, drain::CapacityUnits::from(10));
  drain::JsonValue encoded = node.to_json();
  encoded.set("status", drain::JsonValue("exploded"));
  DRAIN_CHECK(!drain::ResourceNode::from_json(encoded).ok());
}

DRAIN_TEST(adversarial, evidence_reordering_and_replay_are_rejected) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
  DRAIN_CHECK(authority.ok());

  drain::Evidence evidence;
  evidence.seq = drain::EvidenceSeq{10};
  evidence.key.kind = drain::EvidenceKind::FlowQuiesced;
  evidence.key.resource = *drain::ResourceId::parse("a");
  evidence.generation = drain::Generation{1};
  evidence.epoch = engine.epoch();
  evidence.producer = engine.incarnation();
  evidence.observed_at = clock.now();
  DRAIN_CHECK(engine.record_evidence(evidence, clock.now()).ok());

  drain::Evidence replay = evidence;
  DRAIN_CHECK_CODE(engine.record_evidence(replay, clock.now()), drain::ErrorCode::DuplicateIdentity);

  drain::Evidence rewind = evidence;
  rewind.seq = drain::EvidenceSeq{9};
  DRAIN_CHECK_CODE(engine.record_evidence(rewind, clock.now()), drain::ErrorCode::DuplicateIdentity);

  // An observation beyond the tolerated clock skew is rejected outright.
  drain::Evidence future = evidence;
  future.seq = drain::EvidenceSeq{11};
  future.observed_at = drain::TimePoint{clock.now().count() + 3600000000000LL};
  DRAIN_CHECK_CODE(engine.record_evidence(future, clock.now()), drain::ErrorCode::InvalidArgument);

  // An observation inside the tolerated skew is accepted: adjacent runtimes do
  // not share a clock and the tolerance is explicit policy.
  drain::Evidence skewed = evidence;
  skewed.seq = drain::EvidenceSeq{11};
  skewed.observed_at = drain::TimePoint{clock.now().count() + 1000000};
  DRAIN_CHECK(engine.record_evidence(skewed, clock.now()).ok());

  drain::Evidence foreign = evidence;
  foreign.seq = drain::EvidenceSeq{12};
  foreign.epoch = drain::Epoch{99};
  DRAIN_CHECK_CODE(engine.record_evidence(foreign, clock.now()), drain::ErrorCode::StaleEpoch);

  drain::Evidence other_incarnation = evidence;
  other_incarnation.seq = drain::EvidenceSeq{13};
  other_incarnation.producer = drain::IncarnationId{77};
  DRAIN_CHECK_CODE(engine.record_evidence(other_incarnation, clock.now()),
                   drain::ErrorCode::StaleIncarnation);
}

DRAIN_TEST(adversarial, engine_bounds_reject_oversized_requests) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_targets_per_request(4);
  policy.set_max_drains_in_set(2);
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());

  drain::DrainRequest request;
  request.authority = authority->id();
  for (int index = 0; index < 6; ++index) {
    const auto target = target_of("r" + std::to_string(index), "d1");
    DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)),
                                       clock.now()));
    request.targets.push_back(target);
  }
  DRAIN_CHECK_CODE(engine.request_drain_set(request, clock.now()), drain::ErrorCode::BoundsExceeded);

  request.targets.resize(3);
  DRAIN_CHECK_CODE(engine.request_drain_set(request, clock.now()), drain::ErrorCode::BoundsExceeded);

  drain::Obligation empty(drain::ObligationKind::ActiveFlow, holder_of("h"), {});
  DRAIN_CHECK_CODE(engine.admit_obligation(empty, drain::Generation{}, authority->id(), clock.now()),
                   drain::ErrorCode::InvalidArgument);

  std::vector<drain::DrainTarget> too_many;
  for (std::size_t index = 0; index < drain::kMaxDependenciesPerObligation + 1; ++index) {
    too_many.push_back(target_of("t" + std::to_string(index), "d1"));
  }
  drain::Obligation wide(drain::ObligationKind::ActiveFlow, holder_of("h"), too_many);
  DRAIN_CHECK_CODE(engine.admit_obligation(wide, drain::Generation{}, authority->id(), clock.now()),
                   drain::ErrorCode::BoundsExceeded);
}

DRAIN_TEST(adversarial, obligation_documents_reject_unknown_and_unreachable_values) {
  drain::Obligation obligation(drain::ObligationKind::ActiveFlow, holder_of("h"), {target_of("a", "d1")});
  drain::JsonValue encoded = obligation.to_json();

  drain::JsonValue unknown_kind = encoded;
  unknown_kind.set("kind", drain::JsonValue("quantum-flow"));
  DRAIN_CHECK(!drain::Obligation::from_json(unknown_kind).ok());

  drain::JsonValue unknown_state = encoded;
  unknown_state.set("state", drain::JsonValue("levitating"));
  DRAIN_CHECK(!drain::Obligation::from_json(unknown_state).ok());

  drain::JsonValue bad_holder = encoded;
  bad_holder.set("holder", drain::JsonValue("bad holder"));
  DRAIN_CHECK(!drain::Obligation::from_json(bad_holder).ok());

  drain::JsonValue empty_deps = encoded;
  empty_deps.set("dependencies", drain::JsonValue(drain::JsonValue::Array{}));
  DRAIN_CHECK(!drain::Obligation::from_json(empty_deps).ok());

  drain::JsonValue huge_counter = encoded;
  huge_counter.set("reappearances", drain::JsonValue(static_cast<std::uint64_t>(1) << 40));
  DRAIN_CHECK(!drain::Obligation::from_json(huge_counter).ok());

  drain::JsonValue zero_revision = encoded;
  zero_revision.set("revision", drain::JsonValue(static_cast<std::uint64_t>(0)));
  DRAIN_CHECK(!drain::Obligation::from_json(zero_revision).ok());
}

DRAIN_TEST(adversarial, drain_record_from_json_refuses_inconsistent_completions) {
  const auto target = target_of("a", "d1");
  drain::DrainRecord record(drain::DrainId{1}, target, drain::Generation{1}, drain::AuthorityId{1},
                            drain::TimePoint{0});
  drain::JsonValue encoded = record.to_json();
  encoded.set("state", drain::JsonValue("drained"));
  DRAIN_CHECK(!drain::DrainRecord::from_json(encoded).ok());

  encoded.set("remaining_protected_at_completion", drain::JsonValue(static_cast<std::uint64_t>(2)));
  encoded.set("completion_digest", drain::JsonValue(static_cast<std::uint64_t>(1)));
  DRAIN_CHECK(!drain::DrainRecord::from_json(encoded).ok());

  drain::JsonValue blocked = record.to_json();
  blocked.set("state", drain::JsonValue("blocked"));
  DRAIN_CHECK(!drain::DrainRecord::from_json(blocked).ok());

  drain::JsonValue zero_id = record.to_json();
  zero_id.set("id", drain::JsonValue(static_cast<std::uint64_t>(0)));
  DRAIN_CHECK(!drain::DrainRecord::from_json(zero_id).ok());
}

DRAIN_TEST(adversarial, fence_and_authority_documents_reject_bad_entries) {
  drain::AdmissionFence fence;
  const auto target = target_of("a", "d1");
  DRAIN_CHECK(fence.close(target, drain::DrainId{1}, drain::TimePoint{1}, "x").ok());
  drain::JsonValue encoded = fence.to_json();
  encoded.find("entries")->as_array().push_back(encoded.find("entries")->as_array().front());
  drain::AdmissionFence loaded;
  DRAIN_CHECK_CODE(loaded.load_from_json(encoded), drain::ErrorCode::PersistenceCorrupt);

  drain::AuthorityRegistry registry;
  DRAIN_CHECK_OK(registry.install_incarnation(drain::IncarnationId{1}, drain::TimePoint{0}));
  drain::JsonValue tokens = registry.to_json();
  tokens.set("epoch", drain::JsonValue(static_cast<std::uint64_t>(0)));
  drain::AuthorityRegistry restored;
  DRAIN_CHECK_CODE(restored.load_from_json(tokens), drain::ErrorCode::PersistenceCorrupt);
}


// ---------------------------------------------------------------------------
// hardening: boundaries, churn, and repeated lifecycle cycles
// ---------------------------------------------------------------------------

DRAIN_TEST(adversarial, snapshots_from_a_future_format_are_refused) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  const drain::JsonValue snapshot = engine.snapshot();

  drain::DrainEngine other(policy, clock);
  for (std::uint64_t version : {0ULL, 2ULL, 1000ULL, 0xffffffffULL}) {
    drain::JsonValue tampered = snapshot;
    tampered.set("format_version", drain::JsonValue(version));
    DRAIN_CHECK_CODE(other.restore(tampered, drain::IncarnationId{1}, clock.now()),
                     drain::ErrorCode::PersistenceCorrupt);
  }
  DRAIN_CHECK_OK(other.restore(snapshot, drain::IncarnationId{1}, clock.now()));
}

DRAIN_TEST(adversarial, duplicate_entries_in_an_edited_snapshot_are_refused) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)),
                                     clock.now()));
  drain::Obligation draft(drain::ObligationKind::ActiveFlow, holder_of("h"), {target});
  DRAIN_CHECK(engine.admit_obligation(draft, drain::Generation{}, authority->id(), clock.now()).ok());

  const drain::JsonValue snapshot = engine.snapshot();

  drain::DrainEngine other(policy, clock);
  drain::JsonValue duplicated_obligation = snapshot;
  duplicated_obligation.find("obligations")->find("obligations")->as_array().push_back(
      duplicated_obligation.find("obligations")->find("obligations")->as_array().front());
  DRAIN_CHECK_CODE(other.restore(duplicated_obligation, drain::IncarnationId{1}, clock.now()),
                   drain::ErrorCode::PersistenceCorrupt);

  drain::JsonValue duplicated_resource = snapshot;
  duplicated_resource.find("topology")->find("resources")->as_array().push_back(
      duplicated_resource.find("topology")->find("resources")->as_array().front());
  DRAIN_CHECK_CODE(other.restore(duplicated_resource, drain::IncarnationId{1}, clock.now()),
                   drain::ErrorCode::PersistenceCorrupt);

  drain::JsonValue truncated_high_water = snapshot;
  truncated_high_water.find("obligations")->set("high_water", drain::JsonValue(static_cast<std::uint64_t>(0)));
  DRAIN_CHECK_CODE(other.restore(truncated_high_water, drain::IncarnationId{1}, clock.now()),
                   drain::ErrorCode::PersistenceCorrupt);
}

DRAIN_TEST(adversarial, engine_bounds_reject_resource_exhaustion) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_max_obligations(2);
  policy.set_max_evidence_records(2);
  policy.set_max_concurrent_drains_per_domain(1);
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
  const auto target = target_of("r1", "d1");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)),
                                     clock.now()));

  for (int index = 0; index < 2; ++index) {
    drain::Obligation draft(drain::ObligationKind::ActiveFlow,
                            holder_of("h" + std::to_string(index)), {target});
    DRAIN_CHECK(engine.admit_obligation(draft, drain::Generation{}, authority->id(), clock.now()).ok());
  }
  drain::Obligation overflow(drain::ObligationKind::ActiveFlow, holder_of("overflow"), {target});
  DRAIN_CHECK_CODE(engine.admit_obligation(overflow, drain::Generation{}, authority->id(), clock.now()),
                   drain::ErrorCode::BoundsExceeded);

  // Two distinct streams fill the ledger. A third stream cannot be admitted
  // because there is nothing superseded to retire.
  for (std::uint64_t stream = 1; stream <= 2; ++stream) {
    drain::Evidence evidence;
    evidence.seq = drain::EvidenceSeq{1};
    evidence.key.kind = drain::EvidenceKind::FlowQuiesced;
    evidence.key.resource = target.id();
    evidence.key.obligation = drain::ObligationId{stream};
    evidence.epoch = engine.epoch();
    evidence.producer = engine.incarnation();
    evidence.observed_at = clock.now();
    DRAIN_CHECK(engine.record_evidence(evidence, clock.now()).ok());
  }
  drain::Evidence beyond;
  beyond.seq = drain::EvidenceSeq{1};
  beyond.key.kind = drain::EvidenceKind::FlowQuiesced;
  beyond.key.resource = target.id();
  beyond.key.obligation = drain::ObligationId{3};
  beyond.epoch = engine.epoch();
  beyond.producer = engine.incarnation();
  beyond.observed_at = clock.now();
  DRAIN_CHECK_CODE(engine.record_evidence(beyond, clock.now()), drain::ErrorCode::BoundsExceeded);

  // A newer observation on an existing stream supersedes the old one and is
  // therefore always admissible.
  drain::Evidence superseding;
  superseding.seq = drain::EvidenceSeq{2};
  superseding.key.kind = drain::EvidenceKind::FlowQuiesced;
  superseding.key.resource = target.id();
  superseding.key.obligation = drain::ObligationId{1};
  superseding.epoch = engine.epoch();
  superseding.producer = engine.incarnation();
  superseding.observed_at = clock.now();
  DRAIN_CHECK(engine.record_evidence(superseding, clock.now()).ok());
}

DRAIN_TEST(adversarial, cancellation_at_every_boundary_is_legal_or_refused_cleanly) {
  // Cancel from each lifecycle state that can be reached, and assert the drain
  // never claims success and never leaves the fence in an undocumented state.
  for (int boundary = 0; boundary < 6; ++boundary) {
    drain::ManualClock clock;
    drain::DrainPolicy policy = drain::DrainPolicy::defaults();
    policy.set_admission_fence_settle(std::chrono::seconds(0));
    drain::DrainEngine engine(policy, clock);
    ScriptedSink sink(engine);
    sink.mode = ScriptedSink::Mode::RecordOnly;
    engine.set_sink(&sink);
    DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
    auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
    const auto target = target_of("r1", "d1");
    DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)),
                                       clock.now()));
    drain::DrainRequest request;
    request.targets = {target};
    request.authority = authority->id();
    auto set = engine.request_drain_set(request, clock.now());
    DRAIN_CHECK(set.ok());
    const drain::DrainId id = engine.drain_set(*set)->members.front();
    for (int step = 0; step < boundary; ++step) {
      clock.advance(std::chrono::seconds(1));
      (void)engine.advance(clock.now());
    }
    const drain::DrainState before = engine.drain(id)->state();
    const drain::Status cancelled =
        engine.cancel_drain(id, authority->id(), "boundary cancel", clock.now());
    if (before == drain::DrainState::Drained) {
      DRAIN_CHECK_CODE(cancelled, drain::ErrorCode::IllegalTransition);
    } else {
      DRAIN_CHECK_MSG(cancelled.ok(), "cancel from " + std::string(drain::to_string(before)) +
                                          " failed: " + cancelled.error().message());
      for (int step = 0; step < 8; ++step) {
        clock.advance(std::chrono::seconds(1));
        (void)engine.advance(clock.now());
        DRAIN_CHECK(engine.drain(id)->state() != drain::DrainState::Drained);
      }
    }
    const drain::AccountingReport audit = engine.audit(clock.now());
    DRAIN_CHECK_MSG(audit.clean(), audit.render());
  }
}

DRAIN_TEST(adversarial, repeated_runtime_start_stop_cycles_stay_clean) {
  const std::string directory = make_temp_directory("adversarial-cycles");
  const std::string path = directory + "/state.drainlog";
  drain::SystemClock clock;
  drain::ManualClock unused;
  (void)unused;
  drain::IncarnationId previous{};
  for (int cycle = 0; cycle < 8; ++cycle) {
    drain::RuntimeOptions options;
    options.state_path = path;
    drain::Runtime runtime(options, clock);
    DRAIN_CHECK_OK(runtime.start(clock.now()));
    DRAIN_CHECK(runtime.incarnation() > previous);
    previous = runtime.incarnation();
    drain::DrainEngine& engine = runtime.engine();
    auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
    DRAIN_CHECK(authority.ok());
    const auto target = target_of("r" + std::to_string(cycle), "d" + std::to_string(cycle % 2));
    DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(10)),
                                       clock.now()));
    drain::DrainRequest request;
    request.targets = {target};
    request.authority = authority->id();
    (void)engine.request_drain_set(request, clock.now());
    DRAIN_CHECK_OK(runtime.tick(clock.now()));
    DRAIN_CHECK_MSG(engine.audit(clock.now()).clean(), engine.audit(clock.now()).render());
    DRAIN_CHECK_OK(runtime.stop(clock.now()));
  }

  // The snapshot and the boot marker both survive the churn and agree.
  drain::Persistence persistence(path);
  drain::SnapshotInfo info;
  DRAIN_CHECK_OK(persistence.verify(info));
  drain::RuntimeOptions options;
  options.state_path = path;
  drain::Runtime final_runtime(options, clock);
  DRAIN_CHECK_OK(final_runtime.start(clock.now()));
  DRAIN_CHECK(final_runtime.incarnation() > previous);
  DRAIN_CHECK_EQ(final_runtime.engine().drains().size(), 8u);
  DRAIN_CHECK_MSG(final_runtime.engine().audit(clock.now()).clean(),
                  final_runtime.engine().audit(clock.now()).render());
}

DRAIN_TEST(adversarial, evidence_churn_keeps_memory_bounded_and_streams_distinct) {
  drain::EvidenceLedger ledger(64);
  const drain::EvidenceFence fence{drain::IncarnationId{1}, drain::Epoch{1}, drain::TimePoint{1000}};
  for (std::uint64_t stream = 1; stream <= 16; ++stream) {
    for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
      drain::Evidence evidence;
      evidence.seq = drain::EvidenceSeq{sequence};
      evidence.key.kind = drain::EvidenceKind::ObligationReleased;
      evidence.key.obligation = drain::ObligationId{stream};
      evidence.epoch = fence.live_epoch;
      evidence.producer = fence.live_incarnation;
      evidence.observed_at = drain::TimePoint{100};
      DRAIN_CHECK(ledger.record(evidence, fence, true).ok());
    }
  }
  // The bound is on records, not streams: superseded observations are retired
  // as newer ones arrive, so the ledger never exceeds its capacity.
  DRAIN_CHECK_EQ(ledger.size(), ledger.max_records());
  for (std::uint64_t stream = 1; stream <= 16; ++stream) {
    drain::EvidenceKey key;
    key.kind = drain::EvidenceKind::ObligationReleased;
    key.obligation = drain::ObligationId{stream};
    const drain::Evidence* newest = ledger.latest(key);
    DRAIN_CHECK(newest != nullptr);
    DRAIN_CHECK_EQ(newest->seq.value(), 32u);
  }
}

DRAIN_TEST(adversarial, restore_of_a_foreign_snapshot_is_refused) {
  drain::ManualClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{4}, clock.now()));
  const drain::JsonValue snapshot = engine.snapshot();

  drain::DrainEngine other(policy, clock);
  DRAIN_CHECK_CODE(other.restore(snapshot, drain::IncarnationId{9}, clock.now()),
                   drain::ErrorCode::PersistenceCorrupt);

  drain::JsonValue wrong_format = snapshot;
  wrong_format.set("format_version", drain::JsonValue(static_cast<std::uint64_t>(999)));
  DRAIN_CHECK_CODE(other.restore(wrong_format, drain::IncarnationId{4}, clock.now()),
                   drain::ErrorCode::PersistenceCorrupt);

  drain::JsonValue broken_drains = snapshot;
  broken_drains.set("drains", drain::JsonValue("not an array"));
  DRAIN_CHECK_CODE(other.restore(broken_drains, drain::IncarnationId{4}, clock.now()),
                   drain::ErrorCode::PersistenceCorrupt);

  DRAIN_CHECK_OK(other.restore(snapshot, drain::IncarnationId{4}, clock.now()));
}
