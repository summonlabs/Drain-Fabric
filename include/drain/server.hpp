#pragma once

// Drain Fabric -- controller server and wire client.
//
// The controller owns a DrainEngine and serves it over real framed TCP. It is
// also the engine's evacuation sink: when the engine asks an adjacent runtime to
// move an obligation, the server writes an evacuation frame to the agent that
// owns it. Agent replies arrive as ordinary frames and mutate the engine through
// the same public API a local caller would use.
//
// The server never holds its connection registry mutex while calling into the
// engine, never holds a connection mutex while taking the registry mutex, and
// never calls the engine from a socket callback.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "drain/clock.hpp"
#include "drain/engine.hpp"
#include "drain/export.hpp"
#include "drain/result.hpp"
#include "drain/wire.hpp"

namespace drain {

struct ServerOptions {
  std::string bind_host{"127.0.0.1"};
  std::uint16_t port{0};
  std::size_t max_connections{16};
  std::size_t max_worker_threads{4};
  std::size_t max_pending_work{256};
  std::size_t max_frame_bytes{kMaxFramePayloadBytes};
  int accept_wait_ms{20};
  int read_wait_ms{20};
  Duration authority_validity{std::chrono::hours(1)};
};

/// Where a session comes from. Agents may be asked to evacuate obligations;
/// plain clients may only observe and request.
enum class SessionRole : std::uint8_t { Client = 0, Agent = 1 };

DRAIN_API const char* to_string(SessionRole role);
DRAIN_API std::optional<SessionRole> session_role_from_string(std::string_view text);

class ControllerServer final : public EvacuationSink {
 public:
  ControllerServer(DrainEngine& engine, ServerOptions options, Clock& clock);
  ~ControllerServer() override;

  ControllerServer(const ControllerServer&) = delete;
  ControllerServer& operator=(const ControllerServer&) = delete;

  /// Binds and listens. Does not spawn threads.
  Status start();
  Status stop();

  std::uint16_t port() const noexcept { return port_; }
  bool running() const noexcept { return listener_.valid(); }

  /// Accepts at most one connection, services at most one frame per connection,
  /// advances the engine once, and then reaps closed connections.
  Status poll_once();

  /// Deterministic termination condition: serve until the request budget has
  /// been reached or the server stops. No wall-clock deadline is involved.
  Status run_until(std::size_t request_budget);

  Status start_workers(std::size_t count);
  Status stop_workers();

  std::size_t served_requests() const noexcept { return served_requests_.load(std::memory_order_relaxed); }
  std::size_t rejected_frames() const noexcept { return rejected_frames_.load(std::memory_order_relaxed); }
  std::size_t connection_count() const;
  std::size_t agent_count() const;

  // -- EvacuationSink -----------------------------------------------------
  Status on_evacuation_request(const EvacuationRequest& request) override;
  Status on_restoration_request(const RestorationRequest& request) override;
  Status on_admission_reopen(const AdmissionReopenRequest& request) override;

 private:
  struct Connection {
    FrameStream stream{};
    std::mutex mutex{};
    std::string peer{};
    NodeId node{};
    SessionRole role{SessionRole::Client};
    bool greeted{false};
    bool closing{false};
    AuthorityId authority{};
    std::uint32_t last_sequence{0};
  };

  using ConnectionPtr = std::shared_ptr<Connection>;

  std::vector<ConnectionPtr> snapshot_connections() const;
  void register_connection(ConnectionPtr connection);
  void reap_connections();
  Status service_connection(const ConnectionPtr& connection);
  Status handle_frame(const ConnectionPtr& connection, const Frame& frame);
  Status send_frame(const ConnectionPtr& connection, MessageType type, std::uint32_t sequence,
                    JsonValue payload);
  Status send_failure(const ConnectionPtr& connection, std::uint32_t sequence, ErrorCode code,
                      const std::string& message);
  Status push_to_agents(MessageType type, const JsonValue& payload);
  /// Resolves the authority a caller presented. A caller may omit the field to
  /// use its session authority; if it presents one it must present the whole
  /// token, which is then fenced against the live authority line.
  Result<AuthorityId> resolve_authority(const ConnectionPtr& connection, const JsonValue& payload);

  DrainEngine& engine_;
  ServerOptions options_;
  Clock& clock_;

  Socket listener_{};
  std::uint16_t port_{0};
  mutable std::mutex registry_mutex_{};
  std::vector<ConnectionPtr> connections_{};

  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> served_requests_{0};
  std::atomic<std::size_t> rejected_frames_{0};
  std::vector<std::thread> workers_{};
};

/// Client side of the wire protocol. Used by the CLI, by the tests, and by the
/// agent.
class WireClient {
 public:
  WireClient() = default;
  WireClient(FrameStream stream, JsonValue welcome);

  WireClient(WireClient&&) noexcept = default;
  WireClient& operator=(WireClient&&) noexcept = default;
  WireClient(const WireClient&) = delete;
  WireClient& operator=(const WireClient&) = delete;

  static Result<WireClient> connect_to(const std::string& host, std::uint16_t port, SessionRole role,
                                       const std::string& node, std::size_t max_frame_bytes,
                                       int wait_ms);

  bool valid() const noexcept { return stream_.valid(); }
  const JsonValue& welcome() const noexcept { return welcome_; }
  AuthorityId authority() const noexcept { return authority_; }
  const AuthorityToken& session_token() const noexcept { return session_token_; }
  std::uint64_t server_incarnation() const noexcept { return server_incarnation_; }
  std::uint64_t server_epoch() const noexcept { return server_epoch_; }

  Status send(MessageType type, JsonValue payload);
  StreamOutcome receive(Frame& frame, int wait_ms, std::string& error);

  /// Sends a request and waits for the response bearing the same sequence.
  /// Unsolicited push frames are handed to the optional observer.
  Result<JsonValue> call(MessageType type, JsonValue payload, int wait_ms);

  Status close();

  std::uint32_t last_sequence() const noexcept { return sequence_; }

 private:
  FrameStream stream_{};
  JsonValue welcome_{};
  std::uint32_t sequence_{0};
  AuthorityId authority_{};
  AuthorityToken session_token_{};
  std::uint64_t server_incarnation_{0};
  std::uint64_t server_epoch_{0};
};

}  // namespace drain
