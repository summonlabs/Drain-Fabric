// Drain Fabric -- drainagent.
//
// A node agent process. It registers the obligations of an adjacent runtime with
// a controller and answers evacuation requests over real framed TCP. The agent
// holds no Drain Fabric state of its own: it is the proof that the distributed
// path is exercised by independent OS processes rather than by threads.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "drain/agent.hpp"
#include "drain/clock.hpp"
#include "drain/obligation.hpp"
#include "drain/socket.hpp"
#include "drain/version.hpp"

namespace {

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string connect{};
  std::string node{"agent"};
  std::string obligations_file{};
  std::string ready_file{};
  std::size_t exit_after{1};
  std::size_t connect_attempts{200};
  bool fail_evacuations{false};
  bool ignore_evacuations{false};
};

bool parse_endpoint(const std::string& text, std::string& host, std::uint16_t& port) {
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
      "drainagent %s -- Drain Fabric node agent\n"
      "\n"
      "usage: drainagent --connect HOST:PORT [options]\n"
      "\n"
      "  --connect HOST:PORT        controller endpoint (required)\n"
      "  --node ID                  node identity reported at handshake\n"
      "  --obligations FILE         json array of obligations this node holds\n"
      "  --ready-file FILE          written once registration completes\n"
      "  --exit-after N             exit after answering N evacuation requests (0 = never)\n"
      "  --connect-attempts N       bounded connect retries before giving up\n"
      "  --fail                     report evacuation failure instead of releasing\n"
      "  --ignore                   acknowledge requests without releasing\n"
      "  --version                  print the version\n",
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
      std::printf("drainagent %s\n", drain::version_string().c_str());
      return 0;
    } else if (argument == "--connect") {
      options.connect = next("--connect");
    } else if (argument == "--node") {
      options.node = next("--node");
    } else if (argument == "--obligations") {
      options.obligations_file = next("--obligations");
    } else if (argument == "--ready-file") {
      options.ready_file = next("--ready-file");
    } else if (argument == "--exit-after") {
      options.exit_after = static_cast<std::size_t>(std::stoull(next("--exit-after")));
    } else if (argument == "--connect-attempts") {
      options.connect_attempts = static_cast<std::size_t>(std::stoull(next("--connect-attempts")));
    } else if (argument == "--fail") {
      options.fail_evacuations = true;
    } else if (argument == "--ignore") {
      options.ignore_evacuations = true;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", argument.c_str());
      print_usage();
      return 1;
    }
  }
  if (options.connect.empty() || !parse_endpoint(options.connect, options.host, options.port)) {
    std::fprintf(stderr, "error: --connect HOST:PORT is required\n");
    return 1;
  }
  drain::Status started = drain::socket_system_startup();
  if (!started.ok()) {
    std::fprintf(stderr, "error: %s\n", started.error().to_string().c_str());
    return 1;
  }

  drain::AgentOptions agent_options;
  agent_options.host = options.host;
  agent_options.port = options.port;
  agent_options.node = options.node;
  agent_options.fail_evacuations = options.fail_evacuations;
  agent_options.ignore_evacuations = options.ignore_evacuations;

  if (!options.obligations_file.empty()) {
    const std::string text = read_file(options.obligations_file);
    auto parsed = drain::json_parse(text);
    if (!parsed.ok() || !parsed->is_array()) {
      std::fprintf(stderr, "error: the obligations document must be a json array\n");
      return 1;
    }
    for (const auto& entry : parsed->as_array()) {
      auto obligation = drain::Obligation::from_json(entry);
      if (!obligation.ok()) {
        std::fprintf(stderr, "error: obligation rejected: %s\n", obligation.error().message().c_str());
        return 1;
      }
      agent_options.obligations.push_back(*obligation);
    }
  }

  drain::SystemClock clock;
  drain::AgentSession session(agent_options, clock);

  bool connected = false;
  for (std::size_t attempt = 0; attempt < options.connect_attempts; ++attempt) {
    drain::Status status = session.connect();
    if (status.ok()) {
      connected = true;
      break;
    }
    drain::socket_sleep_millis(5);
  }
  if (!connected) {
    std::fprintf(stderr, "error: could not connect to the controller\n");
    return 1;
  }
  drain::Status registered = session.register_obligations();
  if (!registered.ok()) {
    std::fprintf(stderr, "error: obligation registration failed: %s\n",
                 registered.error().to_string().c_str());
    return 1;
  }
  if (!options.ready_file.empty()) {
    std::ofstream ready(options.ready_file, std::ios::binary | std::ios::trunc);
    ready << "registered=" << session.admissions_registered() << "\n";
    ready.flush();
  }
  std::printf("drainagent connected to %s:%u, registered %zu obligation(s)\n", options.host.c_str(),
              static_cast<unsigned>(options.port), session.admissions_registered());
  std::fflush(stdout);

  for (;;) {
    if (options.exit_after != 0 && session.evacuations_handled() >= options.exit_after) {
      break;
    }
    drain::Status step = session.poll_once();
    if (!step.ok()) {
      std::printf("drainagent stopping: %s\n", step.error().to_string().c_str());
      break;
    }
  }
  (void)session.stop();
  std::printf("drainagent answered %zu evacuation request(s)\n", session.evacuations_handled());
  return 0;
}
