#pragma once

// Drain Fabric -- node agent.
//
// An agent runs next to an adjacent runtime (a reroute or migration controller)
// and holds the obligations that runtime owns. It connects to a controller over
// real framed TCP, registers its obligations through the controller's admission
// path, and answers evacuation requests by recording release evidence and
// reporting the obligation state back. The agent never mutates controller state
// directly: every change travels as a fenced frame.

#include <cstdint>
#include <string>
#include <vector>

#include "drain/clock.hpp"
#include "drain/engine.hpp"
#include "drain/export.hpp"
#include "drain/obligation.hpp"
#include "drain/result.hpp"
#include "drain/server.hpp"
#include "drain/wire.hpp"

namespace drain {

struct AgentOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string node{"agent"};
  std::vector<Obligation> obligations{};
  std::size_t max_frame_bytes{kMaxFramePayloadBytes};
  int wait_ms{20};
  /// When true the agent answers an evacuation request by reporting failure
  /// instead of releasing the obligation. Used by the failure-injection tests.
  bool fail_evacuations{false};
  /// When true the agent simulates an adjacent runtime that acknowledges the
  /// request but never releases the obligation.
  bool ignore_evacuations{false};
};

class AgentSession {
 public:
  AgentSession(AgentOptions options, Clock& clock);

  Status connect();
  /// Registers every locally held obligation with the controller.
  Status register_obligations();
  Status poll_once();
  Status run_until(std::size_t handled_budget);
  Status stop();

  bool connected() const noexcept { return client_.valid(); }
  std::size_t evacuations_handled() const noexcept { return evacuations_handled_; }
  std::size_t admissions_registered() const noexcept { return admissions_registered_; }
  std::size_t reopens_handled() const noexcept { return reopens_handled_; }
  const JsonValue& welcome() const noexcept { return client_.welcome(); }
  WireClient& client() noexcept { return client_; }
  const std::vector<ObligationId>& registered_ids() const noexcept { return registered_ids_; }

 private:
  Status handle_evacuation(const Frame& frame);
  Status handle_admission_reopen(const Frame& frame);

  AgentOptions options_;
  Clock& clock_;
  WireClient client_{};
  std::vector<ObligationId> registered_ids_{};
  std::size_t evacuations_handled_{0};
  std::size_t admissions_registered_{0};
  std::size_t reopens_handled_{0};
  bool stopping_{false};
};

}  // namespace drain
