// Drain Fabric -- independent-process transport tests.
//
// These cases are the proof that the distributed path is real: a controller
// daemon and a node agent run as separate OS processes, coordinate over real
// framed TCP on loopback, and are killed and restarted for real. Nothing in this
// file is satisfied by threads or in-memory mocks.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "drain/agent.hpp"
#include "drain/persistence.hpp"
#include "drain/server.hpp"
#include "drain/socket.hpp"
#include "drain/wire.hpp"
#include "fixture.hpp"
#include "process.hpp"
#include "test.hpp"

using namespace drain_test;

#ifndef DRAINFABRIC_TOOLS_DIR
#define DRAINFABRIC_TOOLS_DIR "."
#endif

namespace {

#if defined(_WIN32)
constexpr const char* kExecutableSuffix = ".exe";
#else
constexpr const char* kExecutableSuffix = "";
#endif

std::string tool_path(const std::string& name) {
  return std::string(DRAINFABRIC_TOOLS_DIR) + "/" + name + kExecutableSuffix;
}

std::string topology_document() {
  drain::JsonValue document = drain::JsonValue::object();
  drain::JsonValue resources = drain::JsonValue::array();
  for (int index = 0; index < 3; ++index) {
    const drain::ResourceNode node(target_of("res" + std::to_string(index), "dom0"),
                                   drain::CapacityUnits::from(100));
    resources.array_ref().push_back(node.to_json());
  }
  document.set("resources", std::move(resources));

  drain::JsonValue edges = drain::JsonValue::array();
  for (int left = 0; left < 3; ++left) {
    for (int right = left + 1; right < 3; ++right) {
      drain::JsonValue edge = drain::JsonValue::object();
      edge.set("from", drain::JsonValue("res" + std::to_string(left)));
      edge.set("to", drain::JsonValue("res" + std::to_string(right)));
      edges.array_ref().push_back(std::move(edge));
    }
  }
  document.set("edges", std::move(edges));

  drain::JsonValue paths = drain::JsonValue::array();
  for (int index = 0; index < 3; ++index) {
    drain::JsonValue path = drain::JsonValue::object();
    path.set("id", drain::JsonValue("path" + std::to_string(index)));
    path.set("domain", drain::JsonValue("dom0"));
    drain::JsonValue hops = drain::JsonValue::array();
    hops.array_ref().push_back(drain::JsonValue("res" + std::to_string(index)));
    path.set("hops", std::move(hops));
    paths.array_ref().push_back(std::move(path));
  }
  document.set("paths", std::move(paths));
  return document.dump();
}

/// Waits for a child to signal readiness while continuing to service the
/// in-process controller. The controller does real work (it must answer the
/// agent handshake), so waiting cannot mean "stop polling".
bool pump_until_file(drain::ControllerServer& server, const std::string& path, int budget_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ChildProcess::wait_for_file(path, 0)) {
      return true;
    }
    (void)server.poll_once();
  }
  return ChildProcess::wait_for_file(path, 0);
}

std::uint16_t parse_ready_port(const std::string& contents) {
  const std::size_t marker = contents.find("port=");
  if (marker == std::string::npos) {
    return 0;
  }
  const std::size_t start = marker + 5;
  std::size_t end = start;
  while (end < contents.size() && contents[end] >= '0' && contents[end] <= '9') {
    ++end;
  }
  if (end == start) {
    return 0;
  }
  return static_cast<std::uint16_t>(std::stoul(contents.substr(start, end - start)));
}

}  // namespace

DRAIN_TEST(transport, daemon_handshake_request_and_shutdown_over_real_tcp) {
  const std::string directory = make_temp_directory("transport-daemon");
  const std::string state = directory + "/state.drainlog";
  const std::string topology = directory + "/topology.json";
  const std::string ready = directory + "/ready.txt";
  const std::string log = directory + "/daemon.log";
  DRAIN_CHECK(write_text_file(topology, topology_document()));

  auto child = ChildProcess::spawn(tool_path("drainfabricd"),
                                   {"--listen", "127.0.0.1:0", "--state", state, "--topology", topology,
                                    "--ready-file", ready, "--exit-after-requests", "3"},
                                   "", log);
  DRAIN_CHECK_MSG(child.ok(), child.ok() ? "" : child.error().message());
  DRAIN_CHECK_MSG(ChildProcess::wait_for_file(ready, 20000), "the daemon never became ready: " + child->read_log());

  std::string ready_text;
  DRAIN_CHECK(read_text_file(ready, ready_text));
  const std::uint16_t port = parse_ready_port(ready_text);
  DRAIN_CHECK(port != 0);

  auto client = drain::WireClient::connect_to("127.0.0.1", port, drain::SessionRole::Client, "test-client",
                                              drain::kMaxFramePayloadBytes, 20000);
  DRAIN_CHECK_MSG(client.ok(), client.ok() ? "" : client.error().message());
  DRAIN_CHECK(client->server_incarnation() >= 1);
  DRAIN_CHECK(!client->authority().is_zero());

  drain::JsonValue request = drain::JsonValue::object();
  drain::JsonValue targets = drain::JsonValue::array();
  targets.array_ref().push_back(drain::to_json(target_of("res0", "dom0")));
  request.set("targets", std::move(targets));
  request.set("reason", "transport test");
  request.set("authority", client->session_token().to_json());
  request.set("node", drain::JsonValue("test-client"));
  auto response = client->call(drain::MessageType::DrainRequest, request, 20000);
  DRAIN_CHECK_MSG(response.ok(), response.ok() ? "" : response.error().message());
  DRAIN_CHECK_EQ(response->get_uint("set"), 1u);
  DRAIN_CHECK(response->find("drains") != nullptr);

  drain::JsonValue status_request = drain::JsonValue::object();
  status_request.set("drain", drain::JsonValue(static_cast<std::uint64_t>(0)));
  auto status = client->call(drain::MessageType::StatusRequest, status_request, 20000);
  DRAIN_CHECK(status.ok());
  DRAIN_CHECK(status->find("accounting") != nullptr);
  DRAIN_CHECK_EQ(status->find("drains")->size(), 1u);
  DRAIN_CHECK(status->find("accounting")->get_bool("clean", false));

  auto shutdown = client->call(drain::MessageType::Shutdown, drain::JsonValue::object(), 20000);
  DRAIN_CHECK(shutdown.ok());
  (void)client->close();
  const int exit_code = child->wait();
  DRAIN_CHECK_MSG(exit_code == 0, "daemon exited with " + std::to_string(exit_code) + ": " + child->read_log());

  // The daemon wrote a durable snapshot that the CLI verifier accepts.
  drain::Persistence persistence(state);
  drain::SnapshotInfo info;
  DRAIN_CHECK_OK(persistence.verify(info));
  DRAIN_CHECK(info.sequence >= 1);
}

DRAIN_TEST(transport, killed_daemon_restarts_with_a_fresh_incarnation) {
  const std::string directory = make_temp_directory("transport-restart");
  const std::string state = directory + "/state.drainlog";
  const std::string ready_one = directory + "/ready1.txt";
  const std::string ready_two = directory + "/ready2.txt";
  const std::string log_one = directory + "/daemon1.log";
  const std::string log_two = directory + "/daemon2.log";

  auto first = ChildProcess::spawn(tool_path("drainfabricd"),
                                   {"--listen", "127.0.0.1:0", "--state", state, "--ready-file", ready_one,
                                    "--exit-after-requests", "0"},
                                   "", log_one);
  DRAIN_CHECK_MSG(first.ok(), first.ok() ? "" : first.error().message());
  DRAIN_CHECK_MSG(ChildProcess::wait_for_file(ready_one, 20000), "the first daemon never became ready");

  std::string ready_text;
  DRAIN_CHECK(read_text_file(ready_one, ready_text));
  const std::uint16_t first_port = parse_ready_port(ready_text);
  auto first_client = drain::WireClient::connect_to("127.0.0.1", first_port, drain::SessionRole::Client,
                                                    "probe", drain::kMaxFramePayloadBytes, 20000);
  DRAIN_CHECK(first_client.ok());
  const std::uint64_t first_incarnation = first_client->server_incarnation();
  const std::uint64_t first_epoch = first_client->server_epoch();
  const drain::AuthorityToken stale_authority = first_client->session_token();
  (void)first_client->close();

  // Hard kill: no graceful shutdown, no final save.
  first->terminate();
  DRAIN_CHECK(!first->running());

  auto second = ChildProcess::spawn(tool_path("drainfabricd"),
                                    {"--listen", "127.0.0.1:0", "--state", state, "--ready-file", ready_two,
                                     "--exit-after-requests", "2"},
                                    "", log_two);
  DRAIN_CHECK_MSG(second.ok(), second.ok() ? "" : second.error().message());
  DRAIN_CHECK_MSG(ChildProcess::wait_for_file(ready_two, 20000),
                  "the second daemon never became ready: " + second->read_log());
  DRAIN_CHECK(read_text_file(ready_two, ready_text));
  const std::uint16_t second_port = parse_ready_port(ready_text);
  auto second_client = drain::WireClient::connect_to("127.0.0.1", second_port, drain::SessionRole::Client,
                                                     "probe", drain::kMaxFramePayloadBytes, 20000);
  DRAIN_CHECK(second_client.ok());
  DRAIN_CHECK_MSG(second_client->server_incarnation() > first_incarnation,
                  "the restarted daemon reused an incarnation");
  DRAIN_CHECK_MSG(second_client->server_epoch() > first_epoch, "the restarted daemon reused an epoch");

  // The authority issued by the previous incarnation must be refused.
  drain::JsonValue request = drain::JsonValue::object();
  drain::JsonValue targets = drain::JsonValue::array();
  targets.array_ref().push_back(drain::to_json(target_of("res0", "dom0")));
  request.set("targets", std::move(targets));
  // Presenting the previous incarnation's token must be fenced, even though the
  // restarted daemon happens to hand out the same numeric identifier.
  request.set("authority", stale_authority.to_json());
  auto refused = second_client->call(drain::MessageType::DrainRequest, request, 20000);
  DRAIN_CHECK_MSG(!refused.ok(), "a stale authority from the previous incarnation was accepted");
  DRAIN_CHECK(refused.error().code() == drain::ErrorCode::StaleIncarnation ||
              refused.error().code() == drain::ErrorCode::StaleEpoch ||
              refused.error().code() == drain::ErrorCode::AuthorityRequired);

  auto status = second_client->call(drain::MessageType::StatusRequest, drain::JsonValue::object(), 20000);
  DRAIN_CHECK(status.ok());
  DRAIN_CHECK(status->find("accounting")->get_bool("clean", false));
  (void)second_client->close();
  DRAIN_CHECK_EQ(second->wait(), 0);
}

DRAIN_TEST(transport, malformed_bytes_are_rejected_and_the_session_is_dropped) {
  const std::string directory = make_temp_directory("transport-malformed");
  const std::string state = directory + "/state.drainlog";
  const std::string ready = directory + "/ready.txt";
  const std::string log = directory + "/daemon.log";

  auto child = ChildProcess::spawn(tool_path("drainfabricd"),
                                   {"--listen", "127.0.0.1:0", "--state", state, "--ready-file", ready,
                                    "--exit-after-requests", "1"},
                                   "", log);
  DRAIN_CHECK_MSG(child.ok(), child.ok() ? "" : child.error().message());
  DRAIN_CHECK_MSG(ChildProcess::wait_for_file(ready, 20000), "the daemon never became ready");
  std::string ready_text;
  DRAIN_CHECK(read_text_file(ready, ready_text));
  const std::uint16_t port = parse_ready_port(ready_text);

  auto raw = drain::Socket::connect_to("127.0.0.1", port);
  DRAIN_CHECK(raw.ok());
  const std::vector<unsigned char> garbage(64, 0xab);
  DRAIN_CHECK_OK(raw->send_all(garbage.data(), garbage.size()));

  // The server must drop the session rather than try to interpret the bytes.
  std::string receive_error;
  drain::FrameStream stream(std::move(*raw), drain::kMaxFramePayloadBytes);
  drain::Frame frame;
  const auto outcome = stream.receive_frame(frame, 20000, receive_error);
  DRAIN_CHECK(outcome == drain::StreamOutcome::Closed || outcome == drain::StreamOutcome::Failed ||
              outcome == drain::StreamOutcome::ProtocolError);

  auto client = drain::WireClient::connect_to("127.0.0.1", port, drain::SessionRole::Client, "closer",
                                              drain::kMaxFramePayloadBytes, 20000);
  DRAIN_CHECK(client.ok());
  auto shutdown = client->call(drain::MessageType::Shutdown, drain::JsonValue::object(), 20000);
  DRAIN_CHECK(shutdown.ok());
  (void)client->close();
  DRAIN_CHECK_EQ(child->wait(), 0);
}

DRAIN_TEST(transport, handshake_rejects_a_wrong_protocol_version) {
  const std::string directory = make_temp_directory("transport-version");
  const std::string state = directory + "/state.drainlog";
  const std::string ready = directory + "/ready.txt";
  const std::string log = directory + "/daemon.log";
  auto child = ChildProcess::spawn(tool_path("drainfabricd"),
                                   {"--listen", "127.0.0.1:0", "--state", state, "--ready-file", ready,
                                    "--exit-after-requests", "1"},
                                   "", log);
  DRAIN_CHECK(child.ok());
  DRAIN_CHECK_MSG(ChildProcess::wait_for_file(ready, 20000), "the daemon never became ready");
  std::string ready_text;
  DRAIN_CHECK(read_text_file(ready, ready_text));
  const std::uint16_t port = parse_ready_port(ready_text);

  auto socket = drain::Socket::connect_to("127.0.0.1", port);
  DRAIN_CHECK(socket.ok());
  drain::FrameStream stream(std::move(*socket), drain::kMaxFramePayloadBytes);
  drain::Frame hello;
  hello.type = drain::MessageType::Hello;
  hello.sequence = 1;
  drain::JsonValue payload = drain::JsonValue::object();
  payload.set("protocol_version", drain::JsonValue(static_cast<std::uint64_t>(999)));
  payload.set("role", drain::JsonValue("client"));
  hello.payload = payload;
  DRAIN_CHECK_OK(stream.send_frame(hello));
  drain::Frame response;
  std::string error;
  DRAIN_CHECK_EQ(static_cast<int>(stream.receive_frame(response, 20000, error)),
                 static_cast<int>(drain::StreamOutcome::Frame));
  DRAIN_CHECK_EQ(static_cast<int>(response.type), static_cast<int>(drain::MessageType::Failure));
  DRAIN_CHECK_EQ(response.payload.get_string("code"), std::string("protocol-error"));

  auto client = drain::WireClient::connect_to("127.0.0.1", port, drain::SessionRole::Client, "closer",
                                              drain::kMaxFramePayloadBytes, 20000);
  DRAIN_CHECK(client.ok());
  DRAIN_CHECK(client->call(drain::MessageType::Shutdown, drain::JsonValue::object(), 20000).ok());
  (void)client->close();
  DRAIN_CHECK_EQ(child->wait(), 0);
}

DRAIN_TEST(transport, agent_process_completes_a_drain_across_processes) {
  const std::string directory = make_temp_directory("transport-agent");
  const std::string obligations_file = directory + "/obligations.json";
  const std::string ready = directory + "/agent-ready.txt";
  const std::string log = directory + "/agent.log";

  drain::SystemClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  policy.set_evidence_freshness(std::chrono::minutes(10));
  policy.set_default_grace(std::chrono::minutes(10));
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
  DRAIN_CHECK(authority.ok());

  const auto target = target_of("res-remote", "dom-remote");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(100)),
                                     clock.now()));

  drain::ServerOptions options;
  options.port = 0;
  options.accept_wait_ms = 1;
  options.read_wait_ms = 1;
  drain::ControllerServer server(engine, options, clock);
  DRAIN_CHECK_OK(server.start());
  engine.set_sink(&server);

  drain::Obligation obligation(drain::ObligationKind::ActiveFlow, holder_of("remote-workload"),
                               {target});
  drain::JsonValue obligations = drain::JsonValue::array();
  obligations.array_ref().push_back(obligation.to_json());
  DRAIN_CHECK(write_text_file(obligations_file, obligations.dump()));
  remove_file(ready);

  auto agent = ChildProcess::spawn(
      tool_path("drainagent"),
      {"--connect", "127.0.0.1:" + std::to_string(server.port()), "--node", "node-remote",
       "--obligations", obligations_file, "--ready-file", ready, "--exit-after", "1"},
      "", log);
  DRAIN_CHECK_MSG(agent.ok(), agent.ok() ? "" : agent.error().message());
  DRAIN_CHECK_MSG(pump_until_file(server, ready, 20000),
                  "the agent never registered: " + agent->read_log());

  // The obligation now lives in the controller, admitted by a different OS
  // process over the wire.
  DRAIN_CHECK_EQ(engine.obligations().size(), 1u);
  const auto registered = engine.obligations().front();
  DRAIN_CHECK_EQ(registered.holder().str(), std::string("remote-workload"));

  drain::DrainRequest request;
  request.targets = {target};
  request.reason = "cross-process drain";
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(set.ok());
  const drain::DrainId drain_id = engine.drain_set(*set)->members.front();

  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      DRAIN_CHECK_OK(server.poll_once());
      const auto record = engine.drain(drain_id);
      if (record.has_value() && drain::is_settled(record->state())) {
        break;
      }
    }
  }
  const auto record = engine.drain(drain_id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK_MSG(record->state() == drain::DrainState::Drained,
                  std::string("cross-process drain ended in ") + drain::to_string(record->state()) + " " +
                      record->to_json().dump());
  DRAIN_CHECK_EQ(record->remaining_protected_at_completion(), 0u);
  DRAIN_CHECK_EQ(engine.outstanding_protected_count(target), 0u);
  DRAIN_CHECK_MSG(engine.audit(clock.now()).clean(), engine.audit(clock.now()).render());

  const int exit_code = agent->wait();
  DRAIN_CHECK_MSG(exit_code == 0, "agent exited with " + std::to_string(exit_code) + ": " + agent->read_log());
  DRAIN_CHECK_OK(server.stop());
}

DRAIN_TEST(transport, operator_cli_drives_a_drain_across_separate_processes) {
  const std::string directory = make_temp_directory("transport-cli");
  const std::string state = directory + "/state.drainlog";
  const std::string topology = directory + "/topology.json";
  DRAIN_CHECK(write_text_file(topology, topology_document()));
  int exit_code = 0;

  const auto run = [&](const std::vector<std::string>& arguments, const std::string& log) {
    auto child = ChildProcess::spawn(tool_path("drainctl"), arguments, "", directory + "/" + log);
    DRAIN_CHECK_MSG(child.ok(), child.ok() ? "" : child.error().message());
    const int code = child->wait();
    return std::make_pair(code, child->read_log());
  };

  auto result = run({"--state", state, "init"}, "init.log");
  DRAIN_CHECK_MSG(result.first == 0, result.second);
  result = run({"--state", state, "load-topology", "--file", topology}, "topology.log");
  DRAIN_CHECK_MSG(result.first == 0, result.second);

  result = run({"--state", state, "request", "--target", "resource:res0@dom0", "--reason", "cli workflow"},
               "request.log");
  DRAIN_CHECK_MSG(result.first == 0, result.second);
  DRAIN_CHECK(result.second.find("drain set 1") != std::string::npos);

  for (int attempt = 0; attempt < 12; ++attempt) {
    result = run({"--state", state, "advance", "--steps", "4"}, "advance.log");
    DRAIN_CHECK_MSG(result.first == 0, result.second);
    result = run({"--state", state, "status", "--drain", "1"}, "status.log");
    DRAIN_CHECK_MSG(result.first == 0, result.second);
    if (result.second.find("state=drained") != std::string::npos) {
      break;
    }
  }
  DRAIN_CHECK_MSG(result.second.find("state=drained") != std::string::npos,
                  "the CLI never observed a completed drain: " + result.second);

  result = run({"--state", state, "audit"}, "audit.log");
  DRAIN_CHECK_MSG(result.first == 0, result.second);
  DRAIN_CHECK(result.second.find("violations: 0") != std::string::npos);

  result = run({"--state", state, "explain", "--drain", "1"}, "explain.log");
  DRAIN_CHECK_MSG(result.first == 0, result.second);
  DRAIN_CHECK(result.second.find("remaining-protected=0") != std::string::npos ||
              result.second.find("state=drained") != std::string::npos);

  result = run({"--state", state, "obligations"}, "obligations.log");
  DRAIN_CHECK_MSG(result.first == 0, result.second);

  // The persisted snapshot must verify from a separate process.
  result = run({"--state", state, "verify", "--file", state}, "verify.log");
  DRAIN_CHECK_MSG(result.first == 0, result.second);
  DRAIN_CHECK(result.second.find("snapshot ok") != std::string::npos);

  (void)exit_code;
}

DRAIN_TEST(transport, agent_failure_report_blocks_the_drain_deterministically) {
  const std::string directory = make_temp_directory("transport-agent-failure");
  const std::string obligations_file = directory + "/obligations.json";
  const std::string ready = directory + "/agent-ready.txt";
  const std::string log = directory + "/agent.log";

  drain::SystemClock clock;
  drain::DrainPolicy policy = drain::DrainPolicy::defaults();
  policy.set_admission_fence_settle(std::chrono::seconds(0));
  policy.set_default_grace(std::chrono::minutes(10));
  drain::DrainEngine engine(policy, clock);
  DRAIN_CHECK_OK(engine.install_incarnation(drain::IncarnationId{1}, clock.now()));
  auto authority = engine.issue_authority(drain::DomainId{}, std::chrono::hours(1), clock.now());
  DRAIN_CHECK(authority.ok());
  const auto target = target_of("res-fail", "dom-fail");
  DRAIN_CHECK_OK(engine.add_resource(drain::ResourceNode(target, drain::CapacityUnits::from(100)),
                                     clock.now()));

  drain::ServerOptions options;
  options.port = 0;
  options.accept_wait_ms = 1;
  options.read_wait_ms = 1;
  drain::ControllerServer server(engine, options, clock);
  DRAIN_CHECK_OK(server.start());
  engine.set_sink(&server);

  drain::Obligation obligation(drain::ObligationKind::ActiveFlow, holder_of("remote"), {target});
  drain::JsonValue obligations = drain::JsonValue::array();
  obligations.array_ref().push_back(obligation.to_json());
  DRAIN_CHECK(write_text_file(obligations_file, obligations.dump()));
  remove_file(ready);

  auto agent = ChildProcess::spawn(
      tool_path("drainagent"),
      {"--connect", "127.0.0.1:" + std::to_string(server.port()), "--node", "node-fail",
       "--obligations", obligations_file, "--ready-file", ready, "--exit-after", "1", "--fail"},
      "", log);
  DRAIN_CHECK(agent.ok());
  DRAIN_CHECK_MSG(pump_until_file(server, ready, 20000), "the agent never registered: " + agent->read_log());

  drain::DrainRequest request;
  request.targets = {target};
  request.authority = authority->id();
  auto set = engine.request_drain_set(request, clock.now());
  DRAIN_CHECK(set.ok());
  const drain::DrainId drain_id = engine.drain_set(*set)->members.front();
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      DRAIN_CHECK_OK(server.poll_once());
      const auto record = engine.drain(drain_id);
      if (record.has_value() && record->state() == drain::DrainState::Blocked) {
        break;
      }
    }
  }
  const auto record = engine.drain(drain_id);
  DRAIN_CHECK(record.has_value());
  DRAIN_CHECK_EQ(static_cast<int>(record->state()), static_cast<int>(drain::DrainState::Blocked));
  DRAIN_CHECK(record->has_blocker(drain::BlockerCode::ObligationFailed));
  const drain::Explanation explanation = engine.explain(drain_id);
  DRAIN_CHECK(explanation.render().find("obligation-failed") != std::string::npos);
  DRAIN_CHECK(engine.audit(clock.now()).clean());
  (void)agent->wait();
  DRAIN_CHECK_OK(server.stop());
}
