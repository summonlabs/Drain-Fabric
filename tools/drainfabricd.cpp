// Drain Fabric -- drainfabricd.
//
// The controller daemon. It owns a persistent Runtime, serves the wire protocol
// over real TCP, and terminates on a deterministic condition (a request budget
// or an explicit shutdown frame) rather than on a wall-clock deadline.

#include <atomic>
#include <csignal>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "drain/policy.hpp"
#include "drain/runtime.hpp"
#include "drain/server.hpp"
#include "drain/socket.hpp"
#include "drain/topology.hpp"
#include "drain/version.hpp"

namespace {

std::atomic<bool> g_stop{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT) {
    g_stop.store(true);
    return TRUE;
  }
  return FALSE;
}
#else
void console_handler(int signal) {
  (void)signal;
  g_stop.store(true);
}
#endif

void install_signal_handlers() {
#if defined(_WIN32)
  (void)SetConsoleCtrlHandler(console_handler, TRUE);
#else
  (void)std::signal(SIGINT, console_handler);
  (void)std::signal(SIGTERM, console_handler);
#endif
}

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string state{};
  std::string ready_file{};
  std::string policy_file{};
  std::string topology_file{};
  std::string bind{};
  std::size_t exit_after_requests{0};
  std::size_t workers{0};
  bool once{false};
  bool load_topology{true};
};

bool parse_bind(const std::string& text, std::string& host, std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos) {
    return false;
  }
  host = text.substr(0, colon);
  const std::string port_text = text.substr(colon + 1);
  if (port_text.empty()) {
    return false;
  }
  unsigned value = 0;
  for (const char character : port_text) {
    if (character < '0' || character > '9') {
      return false;
    }
    value = value * 10u + static_cast<unsigned>(character - '0');
    if (value > 65535u) {
      return false;
    }
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

std::string read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::string text;
  stream.seekg(0, std::ios::end);
  text.resize(static_cast<std::size_t>(stream.tellg()));
  stream.seekg(0, std::ios::beg);
  stream.read(text.data(), static_cast<std::streamsize>(text.size()));
  return text;
}

void print_usage() {
  std::printf(
      "drainfabricd %s -- Drain Fabric controller daemon\n"
      "\n"
      "usage: drainfabricd --state FILE [options]\n"
      "\n"
      "  --listen HOST:PORT        bind address (default 127.0.0.1:0, ephemeral port)\n"
      "  --state FILE              persistent state path (required)\n"
      "  --ready-file FILE         written once the listener is up, contains port=N\n"
      "  --policy FILE             policy document applied at start\n"
      "  --topology FILE           topology document applied at start\n"
      "  --no-topology             do not re-apply the topology document\n"
      "  --exit-after-requests N   stop after serving N requests (0 = until shutdown)\n"
      "  --workers N               run N polling workers instead of one loop\n"
      "  --once                    serve exactly one scheduling step and exit\n"
      "  --version                 print the version\n",
      drain::version_string().c_str());
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&](const char* name) -> std::string {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", name);
        std::exit(1);
      }
      return argv[++index];
    };
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else if (argument == "--version") {
      std::printf("drainfabricd %s\n", drain::version_string().c_str());
      return 0;
    } else if (argument == "--listen") {
      options.bind = next("--listen");
    } else if (argument == "--state") {
      options.state = next("--state");
    } else if (argument == "--ready-file") {
      options.ready_file = next("--ready-file");
    } else if (argument == "--policy") {
      options.policy_file = next("--policy");
    } else if (argument == "--topology") {
      options.topology_file = next("--topology");
    } else if (argument == "--no-topology") {
      options.load_topology = false;
    } else if (argument == "--exit-after-requests") {
      options.exit_after_requests = static_cast<std::size_t>(std::stoull(next("--exit-after-requests")));
    } else if (argument == "--workers") {
      options.workers = static_cast<std::size_t>(std::stoull(next("--workers")));
    } else if (argument == "--once") {
      options.once = true;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", argument.c_str());
      print_usage();
      return 1;
    }
  }
  if (options.state.empty()) {
    std::fprintf(stderr, "error: --state is required\n");
    return 1;
  }
  if (!options.bind.empty()) {
    if (!parse_bind(options.bind, options.host, options.port)) {
      std::fprintf(stderr, "error: --listen expects HOST:PORT\n");
      return 1;
    }
  }

  install_signal_handlers();
  drain::Status started = drain::socket_system_startup();
  if (!started.ok()) {
    std::fprintf(stderr, "error: %s\n", started.error().to_string().c_str());
    return 1;
  }

  drain::SystemClock clock;
  drain::RuntimeOptions runtime_options;
  runtime_options.state_path = options.state;
  runtime_options.override_policy = !options.policy_file.empty();
  if (!options.policy_file.empty()) {
    const std::string text = read_file(options.policy_file);
    if (text.empty()) {
      std::fprintf(stderr, "error: cannot read the policy document\n");
      return 1;
    }
    auto parsed = drain::json_parse(text);
    if (!parsed.ok()) {
      std::fprintf(stderr, "error: policy document is not valid json\n");
      return 1;
    }
    auto policy = drain::DrainPolicy::from_json(*parsed);
    if (!policy.ok()) {
      std::fprintf(stderr, "error: policy rejected: %s\n", policy.error().message().c_str());
      return 1;
    }
    runtime_options.policy = *policy;
  }
  drain::Runtime runtime(runtime_options, clock);
  drain::Status boot = runtime.start(clock.now());
  if (!boot.ok()) {
    std::fprintf(stderr, "error: runtime start failed: %s\n", boot.error().to_string().c_str());
    return 1;
  }

  if (options.load_topology && !options.topology_file.empty()) {
    const std::string text = read_file(options.topology_file);
    auto parsed = drain::json_parse(text);
    if (!parsed.ok()) {
      std::fprintf(stderr, "error: topology document is not valid json\n");
      return 1;
    }
    drain::Topology topology;
    drain::Status loaded = topology.load_from_json(*parsed);
    if (!loaded.ok()) {
      std::fprintf(stderr, "error: topology rejected: %s\n", loaded.error().message().c_str());
      return 1;
    }
    for (const auto& [id, node] : topology.resources()) {
      (void)id;
      (void)runtime.engine().add_resource(node, clock.now());
    }
    for (const auto& [id, path] : topology.paths()) {
      (void)id;
      (void)runtime.engine().add_path(path, clock.now());
    }
    const drain::JsonValue* edges = parsed->find("edges");
    if (edges != nullptr && edges->is_array()) {
      for (const auto& entry : edges->as_array()) {
        const auto from = drain::ResourceId::parse(entry.get_string("from"));
        const auto to = drain::ResourceId::parse(entry.get_string("to"));
        if (from.has_value() && to.has_value()) {
          (void)runtime.engine().add_edge(*from, *to, clock.now());
        }
      }
    }
    for (const auto& [id, group] : topology.diversity_groups()) {
      (void)id;
      (void)runtime.engine().add_diversity_group(group, clock.now());
    }
    for (const auto& [id, pool] : topology.capacity_pools()) {
      (void)id;
      (void)runtime.engine().add_capacity_pool(pool, clock.now());
    }
    for (const auto& route : topology.protected_routes()) {
      (void)runtime.engine().add_protected_route(route, clock.now());
    }
  }

  drain::ServerOptions server_options;
  server_options.bind_host = options.host;
  server_options.port = options.port;
  drain::ControllerServer server(runtime.engine(), server_options, clock);
  drain::Status listening = server.start();
  if (!listening.ok()) {
    std::fprintf(stderr, "error: %s\n", listening.error().to_string().c_str());
    return 1;
  }
  runtime.engine().set_sink(&server);

  if (!options.ready_file.empty()) {
    std::ofstream ready(options.ready_file, std::ios::binary | std::ios::trunc);
    ready << "port=" << server.port() << "\n";
    ready << "incarnation=" << runtime.incarnation().value() << "\n";
    ready.flush();
  }
  std::printf("drainfabricd listening on %s:%u incarnation=%llu\n", options.host.c_str(),
              static_cast<unsigned>(server.port()),
              static_cast<unsigned long long>(runtime.incarnation().value()));
  std::fflush(stdout);

  drain::Status served = drain::ok_status();
  if (options.once) {
    served = server.poll_once();
  } else if (options.workers > 0) {
    served = server.start_workers(options.workers);
    while (!g_stop.load() && !server.running()) {
      drain::socket_sleep_millis(2);
    }
    while (!g_stop.load() &&
           (options.exit_after_requests == 0 || server.served_requests() < options.exit_after_requests)) {
      drain::socket_sleep_millis(2);
    }
    (void)server.stop_workers();
  } else {
    while (!g_stop.load()) {
      if (options.exit_after_requests != 0 && server.served_requests() >= options.exit_after_requests) {
        break;
      }
      served = server.poll_once();
      if (!served.ok()) {
        break;
      }
    }
  }

  (void)server.stop();
  drain::Status saved = runtime.stop(clock.now());
  if (!saved.ok()) {
    std::fprintf(stderr, "error: final save failed: %s\n", saved.error().to_string().c_str());
    return 1;
  }
  std::printf("drainfabricd stopped after %zu request(s)\n", server.served_requests());
  return served.ok() ? 0 : 1;
}
