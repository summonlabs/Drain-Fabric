#pragma once

// Drain Fabric -- blocking TCP sockets.
//
// A thin, bounded wrapper over Winsock / POSIX sockets. Every read and write is
// length-exact, every wait is bounded by an explicit millisecond budget supplied
// by the caller, and shutdown closes the descriptor so a blocked accept or
// receive returns promptly. There is no thread, no global state beyond a
// once-only library initialisation, and no hidden allocation.

#include <cstdint>
#include <string>

#include "drain/export.hpp"
#include "drain/result.hpp"

namespace drain {

/// Initialises the platform socket library exactly once per process.
DRAIN_API Status socket_system_startup();

/// Resolves a numeric or dotted host string to an IPv4 loopback-capable address.
DRAIN_API Status socket_parse_host(const std::string& host, std::uint32_t& address_be);

class Socket {
 public:
  using Native = std::intptr_t;

  Socket() = default;
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  bool valid() const noexcept { return handle_ >= 0; }

  /// Binds a listening socket. Port zero asks the platform for a free port,
  /// which is reported back through bound_port.
  static Result<Socket> listen_on(const std::string& host, std::uint16_t port,
                                  std::uint16_t& bound_port, int backlog = 16);

  /// Connects to a peer. Fails with TransportError when the peer refuses.
  static Result<Socket> connect_to(const std::string& host, std::uint16_t port);

  enum class AcceptOutcome { Accepted = 0, TimedOut = 1, Closed = 2, Failed = 3 };

  /// Accepts one connection, waiting at most wait_ms milliseconds.
  AcceptOutcome accept_one(Socket& out, int wait_ms, std::string& error);

  /// Sends exactly length bytes, retrying short writes.
  Status send_all(const void* data, std::size_t length);

  /// Receives up to length bytes. Returns bytes_read; zero means the peer closed.
  Status receive_some(void* data, std::size_t length, std::size_t& bytes_read);

  /// Waits until the socket is readable. Returns Closed when the peer has gone.
  AcceptOutcome wait_readable(int wait_ms, std::string& error);

  /// Half-closes and releases the descriptor. Safe to call repeatedly.
  Status close();

  /// Requests that the peer see end-of-stream without discarding local data.
  Status shutdown_send();

  void set_nodelay(bool enabled);
  void set_keepalive(bool enabled);
  std::uint16_t local_port() const;
  Native native() const noexcept { return handle_; }

 private:
  explicit Socket(Native handle) : handle_(handle) {}

  Native handle_{-1};
};

/// A bounded, interrupted polling wait used by the accept loops.
DRAIN_API void socket_sleep_millis(int millis);

}  // namespace drain
