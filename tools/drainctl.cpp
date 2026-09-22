// Drain Fabric -- drainctl.
//
// The operator-facing command line for Drain Fabric. Every stateful command
// opens the persisted runtime, performs exactly one operation, and saves. The
// CLI never edits state directly: it calls the same public engine API the
// in-process tests use, so anything the CLI can do is auditable in a Decision.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "drain/authority.hpp"
#include "drain/engine.hpp"
#include "drain/persistence.hpp"
#include "drain/runtime.hpp"
#include "drain/topology.hpp"
#include "drain/version.hpp"

namespace {

using drain::ErrorCode;

struct Options {
  std::string state{};
  bool json{false};
  bool help{false};
  std::string command{};
  std::vector<std::string> positional{};
  std::map<std::string, std::string> flags{};

  bool has(const std::string& name) const { return flags.find(name) != flags.end(); }
  std::string get(const std::string& name, const std::string& fallback = {}) const {
    const auto it = flags.find(name);
    return it == flags.end() ? fallback : it->second;
  }
  std::uint64_t number(const std::string& name, std::uint64_t fallback = 0) const {
    const auto it = flags.find(name);
    if (it == flags.end() || it->second.empty()) {
      return fallback;
    }
    std::uint64_t value = 0;
    for (const char character : it->second) {
      if (character < '0' || character > '9') {
        return fallback;
      }
      value = value * 10 + static_cast<std::uint64_t>(character - '0');
    }
    return value;
  }
};

void print_usage() {
  std::printf(
      "drainctl %s -- Drain Fabric operator CLI\n"
      "\n"
      "usage: drainctl [--state FILE] [--json] <command> [arguments]\n"
      "\n"
      "commands:\n"
      "  init                                  create an empty persistent state\n"
      "  load-topology --file F | --json J     install a topology document\n"
      "  show-topology                         print the installed topology\n"
      "  admit --holder H --deps T[,T...]      admit an obligation\n"
      "        [--kind K] [--protection P] [--grace-ns N] [--group G] [--capacity N]\n"
      "  release --obligation N [--advance N]  report an obligation as released\n"
      "  request --target T [--target T...]    request a drain set\n"
      "        [--reason R] [--nonce N]\n"
      "  advance [--steps N]                   run scheduling passes\n"
      "  status [--drain N]                    show drain state and accounting\n"
      "  explain --drain N                     explain why a drain is where it is\n"
      "  obligations [--target T]              list outstanding protected work\n"
      "  cancel --drain N [--reason R]         cancel a drain\n"
      "  restore --drain N [--reason R]        return a target to service\n"
      "  audit                                 run the accounting closure audit\n"
      "  verify --file F                       integrity-check a snapshot file\n"
      "  snapshot --out F                      write the current snapshot to F\n"
      "  version                               print the version\n"
      "\n"
      "Exit codes: 0 success, 1 usage error, 2 operation rejected, 3 not found,\n"
      "            4 stale fence, 5 integrity failure.\n",
      drain::version_string().c_str());
}

int exit_code_for(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return 0;
    case ErrorCode::NotFound: return 3;
    case ErrorCode::StaleAuthority:
    case ErrorCode::StaleEpoch:
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleIncarnation:
    case ErrorCode::StaleEvidence:
    case ErrorCode::StaleRevision:
    case ErrorCode::AdmissionClosed: return 4;
    case ErrorCode::PersistenceCorrupt:
    case ErrorCode::PersistenceIo: return 5;
    default: return 2;
  }
}

int report(const Options& options, const drain::Status& status) {
  if (status.ok()) {
    return 0;
  }
  if (options.json) {
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(false));
    value.set("code", drain::JsonValue(std::string(drain::to_string(status.code()))));
    value.set("message", drain::JsonValue(status.error().message()));
    std::printf("%s\n", value.dump().c_str());
  } else {
    std::fprintf(stderr, "error: %s\n", status.error().to_string().c_str());
  }
  return exit_code_for(status.code());
}

void emit(const Options& options, const drain::JsonValue& value, const std::string& text) {
  if (options.json) {
    std::printf("%s\n", value.dump(2).c_str());
  } else {
    std::printf("%s", text.c_str());
  }
}

Options parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument == "--json") {
      options.json = true;
      continue;
    }
    if (argument.rfind("--", 0) == 0) {
      const std::size_t equals = argument.find('=');
      if (equals != std::string::npos) {
        options.flags[argument.substr(2, equals - 2)] = argument.substr(equals + 1);
        continue;
      }
      const std::string name = argument.substr(2);
      if (index + 1 < argc && argv[index + 1][0] != '-') {
        options.flags[name] = argv[++index];
      } else {
        options.flags[name] = "true";
      }
      continue;
    }
    if (options.command.empty()) {
      options.command = argument;
    } else {
      options.positional.push_back(argument);
    }
  }
  options.state = options.get("state");
  return options;
}

std::vector<drain::DrainTarget> parse_target_list(const std::string& text) {
  std::vector<drain::DrainTarget> targets;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string piece = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!piece.empty()) {
      auto parsed = drain::DrainTarget::parse(piece);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "error: '%s' is not a valid drain target (kind:id@domain)\n", piece.c_str());
        std::exit(1);
      }
      targets.push_back(*parsed);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return targets;
}

drain::NodeId parse_node(const std::string& text) {
  if (text.empty()) {
    return drain::NodeId{};
  }
  auto parsed = drain::NodeId::parse(text);
  if (!parsed.has_value()) {
    std::fprintf(stderr, "error: '%s' is not a valid node identity\n", text.c_str());
    std::exit(1);
  }
  return *parsed;
}

std::string read_file(const std::string& path, bool& ok) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    ok = false;
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  ok = true;
  return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse(argc, argv);
  if (options.help || options.command.empty()) {
    print_usage();
    return options.command.empty() && !options.help ? 1 : 0;
  }
  if (options.command == "version") {
    std::printf("drainctl %s (snapshot format %u, wire protocol %u)\n", drain::version_string().c_str(),
                static_cast<unsigned>(drain::kSnapshotFormatVersion),
                static_cast<unsigned>(drain::kWireProtocolVersion));
    return 0;
  }
  if (options.command == "verify") {
    const std::string path = options.get("file", options.state);
    if (path.empty()) {
      std::fprintf(stderr, "error: verify requires --file\n");
      return 1;
    }
    drain::Persistence persistence(path);
    drain::SnapshotInfo info;
    drain::Status verified = persistence.verify(info);
    if (!verified.ok()) {
      return report(options, verified);
    }
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    value.set("format_version", drain::JsonValue(static_cast<std::uint64_t>(info.format_version)));
    value.set("sequence", drain::JsonValue(info.sequence));
    value.set("incarnation", drain::JsonValue(info.incarnation.value()));
    value.set("payload_bytes", drain::JsonValue(info.payload_bytes));
    value.set("from_backup", drain::JsonValue(info.from_backup));
    std::string text = "snapshot ok: format " + std::to_string(info.format_version) + ", sequence " +
                       std::to_string(info.sequence) + ", incarnation " +
                       std::to_string(info.incarnation.value()) + ", payload " +
                       std::to_string(info.payload_bytes) + " bytes" +
                       (info.from_backup ? " (recovered from backup)" : "") + "\n";
    emit(options, value, text);
    return 0;
  }

  if (options.state.empty()) {
    std::fprintf(stderr, "error: this command requires --state FILE\n");
    return 1;
  }

  drain::SystemClock clock;
  drain::RuntimeOptions runtime_options;
  runtime_options.state_path = options.state;
  drain::Runtime runtime(runtime_options, clock);
  const drain::TimePoint now = clock.now();
  drain::Status started = runtime.start(now);
  if (!started.ok()) {
    return report(options, started);
  }
  drain::DrainEngine& engine = runtime.engine();

  if (options.command == "init") {
    const std::string path = options.get("file", options.state);
    (void)path;
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    value.set("incarnation", drain::JsonValue(runtime.incarnation().value()));
    value.set("boot_id", drain::JsonValue(runtime.boot_id()));
    emit(options, value, "state initialised at " + options.state + " (incarnation " +
                             std::to_string(runtime.incarnation().value()) + ")\n");
    return report(options, runtime.save(now));
  }

  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
  if (!authority.ok()) {
    return report(options, authority.error());
  }
  const drain::AuthorityId authority_id = authority->id();

  if (options.command == "load-topology") {
    std::string document = options.get("json");
    if (document.empty()) {
      bool ok = false;
      document = read_file(options.get("file"), ok);
      if (!ok) {
        std::fprintf(stderr, "error: cannot read the topology document\n");
        return 1;
      }
    }
    auto parsed = drain::json_parse(document);
    if (!parsed.ok()) {
      std::fprintf(stderr, "error: topology document is not valid json: %s\n",
                   parsed.error().message().c_str());
      return 1;
    }
    drain::Topology topology;
    drain::Status loaded = topology.load_from_json(*parsed);
    if (!loaded.ok()) {
      return report(options, loaded);
    }
    for (const auto& [id, node] : topology.resources()) {
      (void)id;
      drain::Status added = engine.add_resource(node, clock.now());
      if (!added.ok()) {
        return report(options, added);
      }
    }
    for (const auto& [id, path] : topology.paths()) {
      (void)id;
      drain::Status added = engine.add_path(path, clock.now());
      if (!added.ok()) {
        return report(options, added);
      }
    }
    for (const auto& entry : parsed->find("edges") != nullptr ? parsed->find("edges")->as_array()
                                                              : drain::JsonValue::Array{}) {
      const auto from = drain::ResourceId::parse(entry.get_string("from"));
      const auto to = drain::ResourceId::parse(entry.get_string("to"));
      if (from.has_value() && to.has_value()) {
        (void)engine.add_edge(*from, *to, clock.now());
      }
    }
    for (const auto& [id, group] : topology.diversity_groups()) {
      (void)id;
      drain::Status added = engine.add_diversity_group(group, clock.now());
      if (!added.ok()) {
        return report(options, added);
      }
    }
    for (const auto& [id, pool] : topology.capacity_pools()) {
      (void)id;
      drain::Status added = engine.add_capacity_pool(pool, clock.now());
      if (!added.ok()) {
        return report(options, added);
      }
    }
    for (const auto& route : topology.protected_routes()) {
      drain::Status added = engine.add_protected_route(route, clock.now());
      if (!added.ok()) {
        return report(options, added);
      }
    }
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    value.set("resources", drain::JsonValue(static_cast<std::uint64_t>(topology.resource_count())));
    emit(options, value,
         "topology loaded: " + std::to_string(topology.resource_count()) + " resource(s)\n");
    return report(options, runtime.save(clock.now()));
  }

  if (options.command == "show-topology") {
    emit(options, engine.topology_snapshot().to_json(), engine.topology_snapshot().to_json().dump(2) + "\n");
    return 0;
  }

  if (options.command == "admit") {
    const std::string holder = options.get("holder");
    const std::string deps = options.get("deps");
    if (holder.empty() || deps.empty()) {
      std::fprintf(stderr, "error: admit requires --holder and --deps\n");
      return 1;
    }
    auto holder_id = drain::HolderId::parse(holder);
    if (!holder_id.has_value()) {
      std::fprintf(stderr, "error: malformed holder identity\n");
      return 1;
    }
    const std::string kind_text = options.get("kind", "active-flow");
    auto kind = drain::obligation_kind_from_string(kind_text);
    if (!kind.has_value()) {
      std::fprintf(stderr, "error: unknown obligation kind '%s'\n", kind_text.c_str());
      return 1;
    }
    drain::Obligation draft(*kind, *holder_id, parse_target_list(deps));
    const std::string protection_text = options.get("protection", "protected");
    auto protection = drain::protection_class_from_string(protection_text);
    if (!protection.has_value()) {
      std::fprintf(stderr, "error: unknown protection class '%s'\n", protection_text.c_str());
      return 1;
    }
    draft.set_protection(*protection);
    if (options.has("grace-ns")) {
      draft.set_grace(drain::Duration{static_cast<std::int64_t>(options.number("grace-ns"))});
    }
    if (options.has("capacity")) {
      draft.set_capacity(drain::CapacityUnits::from(options.number("capacity")));
    }
    if (options.has("group")) {
      auto group = drain::GroupId::parse(options.get("group"));
      if (!group.has_value()) {
        std::fprintf(stderr, "error: malformed group identity\n");
        return 1;
      }
      draft.set_group(*group);
    }
    drain::Generation presented{options.number("generation", 0)};
    auto admitted = engine.admit_obligation(draft, presented, authority_id, clock.now());
    if (!admitted.ok()) {
      return report(options, admitted.error());
    }
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    value.set("obligation", drain::JsonValue(admitted->id().value()));
    emit(options, value, "obligation " + std::to_string(admitted->id().value()) + " admitted\n");
    return report(options, runtime.save(clock.now()));
  }

  if (options.command == "release") {
    const std::uint64_t obligation_id = options.number("obligation");
    if (obligation_id == 0) {
      std::fprintf(stderr, "error: release requires --obligation\n");
      return 1;
    }
    auto obligation = engine.obligation(drain::ObligationId{obligation_id});
    if (!obligation.has_value()) {
      std::fprintf(stderr, "error: no such obligation\n");
      return 3;
    }
    // A release observation is only useful while the incarnation that made it is
    // alive, so the CLI records it and drives the pending transitions in the
    // same invocation.
    drain::Evidence evidence;
    evidence.key.kind = drain::EvidenceKind::ObligationReleased;
    evidence.key.obligation = drain::ObligationId{obligation_id};
    evidence.key.drain = drain::DrainId{options.number("drain", 0)};
    evidence.seq = drain::EvidenceSeq{options.number("seq", 1)};
    evidence.generation = drain::Generation{options.number("generation", 0)};
    evidence.epoch = engine.epoch();
    evidence.producer = engine.incarnation();
    evidence.observed_at = clock.now();
    evidence.healthy = true;
    evidence.detail = "released by drainctl";
    auto recorded = engine.record_evidence(evidence, clock.now());
    if (!recorded.ok()) {
      return report(options, recorded.error());
    }
    drain::ObligationReport report_message;
    report_message.obligation = drain::ObligationId{obligation_id};
    report_message.expected_revision = obligation->revision();
    report_message.next = drain::ObligationState::Released;
    report_message.evidence = recorded->seq;
    report_message.drain = evidence.key.drain;
    report_message.generation = evidence.generation;
    report_message.note = "drainctl release";
    drain::Status reported = engine.report_obligation(report_message, clock.now());
    if (!reported.ok()) {
      return report(options, reported);
    }
    const std::uint64_t steps = options.number("advance", 6);
    for (std::uint64_t step = 0; step < steps; ++step) {
      (void)engine.advance(clock.now());
    }
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    value.set("evidence", drain::JsonValue(recorded->seq.value()));
    emit(options, value, "obligation " + std::to_string(obligation_id) + " released\n");
    return report(options, runtime.save(clock.now()));
  }

  if (options.command == "request") {
    std::vector<drain::DrainTarget> targets;
    const auto it = options.flags.find("target");
    if (it != options.flags.end()) {
      targets = parse_target_list(it->second);
    }
    for (const auto& positional : options.positional) {
      auto parsed = drain::DrainTarget::parse(positional);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "error: '%s' is not a valid drain target\n", positional.c_str());
        return 1;
      }
      targets.push_back(*parsed);
    }
    if (targets.empty()) {
      std::fprintf(stderr, "error: request requires --target\n");
      return 1;
    }
    drain::DrainRequest request;
    request.targets = std::move(targets);
    request.reason = options.get("reason", "drainctl request");
    request.authority = authority_id;
    request.nonce = drain::RequestNonce{options.number("nonce", 0)};
    request.node = parse_node(options.get("node"));
    auto set = engine.request_drain_set(request, clock.now());
    if (!set.ok()) {
      return report(options, set.error());
    }
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    value.set("set", drain::JsonValue(set->value()));
    const auto stored = engine.drain_set(*set);
    drain::JsonValue drains = drain::JsonValue::array();
    std::string text = "drain set " + std::to_string(set->value()) + ":";
    if (stored.has_value()) {
      for (const drain::DrainId member : stored->members) {
        drains.array_ref().push_back(drain::JsonValue(member.value()));
        text.append(" " + std::to_string(member.value()));
      }
    }
    value.set("drains", std::move(drains));
    emit(options, value, text + "\n");
    return report(options, runtime.save(clock.now()));
  }

  if (options.command == "advance") {
    const std::uint64_t steps = options.number("steps", 1);
    for (std::uint64_t step = 0; step < steps; ++step) {
      drain::Status advanced = engine.advance(clock.now());
      if (!advanced.ok()) {
        return report(options, advanced);
      }
    }
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    value.set("steps", drain::JsonValue(steps));
    emit(options, value, "advanced " + std::to_string(steps) + " step(s)\n");
    return report(options, runtime.save(clock.now()));
  }

  if (options.command == "status") {
    const std::uint64_t wanted = options.number("drain");
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    drain::JsonValue drains = drain::JsonValue::array();
    std::string text;
    if (wanted != 0) {
      auto record = engine.drain(drain::DrainId{wanted});
      if (!record.has_value()) {
        std::fprintf(stderr, "error: no such drain\n");
        return 3;
      }
      drains.array_ref().push_back(record->to_json());
      text.append("drain " + std::to_string(wanted) + " target=" + record->target().to_string() +
                  " state=" + drain::to_string(record->state()) +
                  " generation=" + drain::to_string(record->generation()) + "\n");
      for (const auto& blocker : record->blockers()) {
        text.append("  blocker: " + blocker.render() + "\n");
      }
    } else {
      for (const auto& record : engine.drains()) {
        drains.array_ref().push_back(record.to_json());
        text.append("drain " + std::to_string(record.id().value()) + " target=" +
                    record.target().to_string() + " state=" + drain::to_string(record.state()) +
                    " generation=" + drain::to_string(record.generation()) + "\n");
      }
      if (engine.drains().empty()) {
        text.append("no drains\n");
      }
    }
    value.set("drains", std::move(drains));
    value.set("accounting", engine.audit(clock.now()).to_json());
    if (!options.json) {
      text.append(engine.audit(clock.now()).render());
    }
    emit(options, value, text);
    return 0;
  }

  if (options.command == "obligations") {
    std::vector<drain::Obligation> obligations;
    if (options.has("target")) {
      auto parsed = drain::DrainTarget::parse(options.get("target"));
      if (!parsed.has_value()) {
        std::fprintf(stderr, "error: malformed target\n");
        return 1;
      }
      obligations = engine.outstanding_obligations(*parsed);
    } else {
      obligations = engine.obligations();
    }
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(true));
    drain::JsonValue items = drain::JsonValue::array();
    std::string text;
    for (const auto& obligation : obligations) {
      items.array_ref().push_back(obligation.to_json());
      text.append(obligation.label() + " state=" + drain::to_string(obligation.state()) +
                  " protection=" + drain::to_string(obligation.protection()) +
                  " deps=" + std::to_string(obligation.dependencies().size()) + "\n");
    }
    value.set("obligations", std::move(items));
    emit(options, value, text);
    return 0;
  }

  if (options.command == "explain") {
    const std::uint64_t wanted = options.number("drain");
    if (wanted == 0) {
      std::fprintf(stderr, "error: explain requires --drain\n");
      return 1;
    }
    const drain::Explanation explanation = engine.explain(drain::DrainId{wanted});
    drain::JsonValue value = drain::JsonValue::object();
    value.set("ok", drain::JsonValue(explanation.found));
    value.set("explanation", explanation.to_json());
    emit(options, value, explanation.render());
    return explanation.found ? 0 : 3;
  }

  if (options.command == "cancel") {
    const std::uint64_t wanted = options.number("drain");
    if (wanted == 0) {
      std::fprintf(stderr, "error: cancel requires --drain\n");
      return 1;
    }
    drain::Status cancelled =
        engine.cancel_drain(drain::DrainId{wanted}, authority_id, options.get("reason", "drainctl cancel"),
                            clock.now());
    if (!cancelled.ok()) {
      return report(options, cancelled);
    }
    emit(options, [] {
      drain::JsonValue value = drain::JsonValue::object();
      value.set("ok", drain::JsonValue(true));
      return value;
    }(), "drain " + std::to_string(wanted) + " cancelled\n");
    return report(options, runtime.save(clock.now()));
  }

  if (options.command == "restore") {
    const std::uint64_t wanted = options.number("drain");
    if (wanted == 0) {
      std::fprintf(stderr, "error: restore requires --drain\n");
      return 1;
    }
    drain::Status restored =
        engine.restore_drain(drain::DrainId{wanted}, authority_id, options.get("reason", "drainctl restore"),
                             clock.now());
    if (!restored.ok()) {
      return report(options, restored);
    }
    const std::uint64_t steps = options.number("steps", 4);
    for (std::uint64_t step = 0; step < steps; ++step) {
      (void)engine.advance(clock.now());
    }
    emit(options, [] {
      drain::JsonValue value = drain::JsonValue::object();
      value.set("ok", drain::JsonValue(true));
      return value;
    }(), "drain " + std::to_string(wanted) + " restored\n");
    return report(options, runtime.save(clock.now()));
  }

  if (options.command == "audit") {
    const drain::AccountingReport audit = engine.audit(clock.now());
    drain::JsonValue value = audit.to_json();
    emit(options, value, audit.render());
    return audit.clean() ? 0 : 2;
  }

  if (options.command == "snapshot") {
    const std::string out = options.get("out", options.state + ".snapshot.json");
    const std::string text = engine.snapshot().dump(2);
    std::ofstream stream(out, std::ios::binary | std::ios::trunc);
    if (!stream) {
      std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
      return 1;
    }
    stream << text;
    stream.flush();
    std::printf("snapshot written to %s (%zu bytes)\n", out.c_str(), text.size());
    return 0;
  }

  std::fprintf(stderr, "error: unknown command '%s'\n", options.command.c_str());
  print_usage();
  return 1;
}
