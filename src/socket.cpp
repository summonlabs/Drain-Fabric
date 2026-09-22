#include "drain/socket.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace drain {
namespace {

#if defined(_WIN32)
constexpr Socket::Native kInvalid = static_cast<Socket::Native>(INVALID_SOCKET);
#else
constexpr Socket::Native kInvalid = -1;
#endif

int last_socket_error() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

#if defined(_WIN32)
using SelectFd = SOCKET;
#else
using SelectFd = int;
#endif

SelectFd select_fd(Socket::Native handle) { return static_cast<SelectFd>(handle); }

bool is_would_block(int code) {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT;
#else
  return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
#endif
}

std::string socket_error_text(int code) {
#if defined(_WIN32)
  return "winsock error " + std::to_string(code);
#else
  return std::string(std::strerror(code));
#endif
}

}  // namespace

Status socket_system_startup() {
#if defined(_WIN32)
  static const Status once = []() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      return fail(ErrorCode::TransportError, "WSAStartup failed with " + std::to_string(result));
    }
    return ok_status();
  }();
  return once;
#else
  return ok_status();
#endif
}

Status socket_parse_host(const std::string& host, std::uint32_t& address_be) {
  if (host.empty() || host == "localhost") {
    address_be = htonl(INADDR_LOOPBACK);
    return ok_status();
  }
  in_addr parsed{};
  if (inet_pton(AF_INET, host.c_str(), &parsed) != 1) {
    return fail(ErrorCode::InvalidArgument, "host must be a dotted IPv4 address or 'localhost'");
  }
  address_be = parsed.s_addr;
  return ok_status();
}

Socket::~Socket() { (void)close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalid; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    (void)close();
    handle_ = other.handle_;
    other.handle_ = kInvalid;
  }
  return *this;
}

Result<Socket> Socket::listen_on(const std::string& host, std::uint16_t port, std::uint16_t& bound_port,
                                 int backlog) {
  Status started = socket_system_startup();
  if (!started.ok()) {
    return started.error();
  }
  std::uint32_t address = 0;
  Status parsed = socket_parse_host(host, address);
  if (!parsed.ok()) {
    return parsed.error();
  }
  const Native handle = static_cast<Native>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (handle == kInvalid) {
    return Error(ErrorCode::TransportError, "socket() failed: " + socket_error_text(last_socket_error()));
  }
  Socket socket(handle);
  int reuse = 1;
  (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     static_cast<int>(sizeof(reuse)));

  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  endpoint.sin_addr.s_addr = address;
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&endpoint), static_cast<int>(sizeof(endpoint))) != 0) {
    return Error(ErrorCode::TransportError, "bind() failed: " + socket_error_text(last_socket_error()));
  }
  if (::listen(handle, backlog) != 0) {
    return Error(ErrorCode::TransportError, "listen() failed: " + socket_error_text(last_socket_error()));
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(actual));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(actual));
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&actual), &length) != 0) {
    return Error(ErrorCode::TransportError, "getsockname() failed: " + socket_error_text(last_socket_error()));
  }
  bound_port = ntohs(actual.sin_port);
  socket.set_nodelay(true);
  return socket;
}

Result<Socket> Socket::connect_to(const std::string& host, std::uint16_t port) {
  Status started = socket_system_startup();
  if (!started.ok()) {
    return started.error();
  }
  std::uint32_t address = 0;
  Status parsed = socket_parse_host(host, address);
  if (!parsed.ok()) {
    return parsed.error();
  }
  const Native handle = static_cast<Native>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (handle == kInvalid) {
    return Error(ErrorCode::TransportError, "socket() failed: " + socket_error_text(last_socket_error()));
  }
  Socket socket(handle);
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  endpoint.sin_addr.s_addr = address;
  if (::connect(handle, reinterpret_cast<const sockaddr*>(&endpoint), static_cast<int>(sizeof(endpoint))) != 0) {
    return Error(ErrorCode::TransportError, "connect() failed: " + socket_error_text(last_socket_error()));
  }
  socket.set_nodelay(true);
  return socket;
}

Socket::AcceptOutcome Socket::accept_one(Socket& out, int wait_ms, std::string& error) {
  if (!valid()) {
    error = "listening socket is closed";
    return AcceptOutcome::Closed;
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(select_fd(handle_), &read_set);
  timeval timeout{};
  timeout.tv_sec = wait_ms / 1000;
  timeout.tv_usec = (wait_ms % 1000) * 1000;
#if defined(_WIN32)
  const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
  const int ready = ::select(static_cast<int>(handle_) + 1, &read_set, nullptr, nullptr, &timeout);
#endif
  if (ready == 0) {
    return AcceptOutcome::TimedOut;
  }
  if (ready < 0) {
    const int code = last_socket_error();
    if (is_would_block(code)) {
      return AcceptOutcome::TimedOut;
    }
    error = "select() failed: " + socket_error_text(code);
    return AcceptOutcome::Closed;
  }
  sockaddr_in peer{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(peer));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(peer));
#endif
  const Native accepted = static_cast<Native>(::accept(handle_, reinterpret_cast<sockaddr*>(&peer), &length));
  if (accepted == kInvalid) {
    const int code = last_socket_error();
    if (is_would_block(code)) {
      return AcceptOutcome::TimedOut;
    }
    error = "accept() failed: " + socket_error_text(code);
    return AcceptOutcome::Failed;
  }
  out = Socket(accepted);
  out.set_nodelay(true);
  out.set_keepalive(true);
  return AcceptOutcome::Accepted;
}

Status Socket::send_all(const void* data, std::size_t length) {
  if (!valid()) {
    return fail(ErrorCode::TransportError, "send on a closed socket");
  }
  const auto* bytes = static_cast<const char*>(data);
  std::size_t sent = 0;
  while (sent < length) {
    const int chunk = static_cast<int>(std::min<std::size_t>(length - sent, 1u << 20));
    const int written = ::send(static_cast<Native>(handle_), bytes + sent, chunk, 0);
    if (written <= 0) {
      const int code = last_socket_error();
      if (is_would_block(code)) {
        continue;
      }
      return fail(ErrorCode::TransportError, "send() failed: " + socket_error_text(code));
    }
    sent += static_cast<std::size_t>(written);
  }
  return ok_status();
}

Status Socket::receive_some(void* data, std::size_t length, std::size_t& bytes_read) {
  bytes_read = 0;
  if (!valid()) {
    return fail(ErrorCode::TransportError, "receive on a closed socket");
  }
  const int chunk = static_cast<int>(std::min<std::size_t>(length, 1u << 20));
  const int received = ::recv(static_cast<Native>(handle_), static_cast<char*>(data), chunk, 0);
  if (received == 0) {
    return ok_status();
  }
  if (received < 0) {
    const int code = last_socket_error();
    if (is_would_block(code)) {
      return ok_status();
    }
    return fail(ErrorCode::TransportError, "recv() failed: " + socket_error_text(code));
  }
  bytes_read = static_cast<std::size_t>(received);
  return ok_status();
}

Socket::AcceptOutcome Socket::wait_readable(int wait_ms, std::string& error) {
  if (!valid()) {
    error = "socket is closed";
    return AcceptOutcome::Closed;
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(select_fd(handle_), &read_set);
  timeval timeout{};
  timeout.tv_sec = wait_ms / 1000;
  timeout.tv_usec = (wait_ms % 1000) * 1000;
#if defined(_WIN32)
  const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
#else
  const int ready = ::select(static_cast<int>(handle_) + 1, &read_set, nullptr, nullptr, &timeout);
#endif
  if (ready == 0) {
    return AcceptOutcome::TimedOut;
  }
  if (ready < 0) {
    const int code = last_socket_error();
    if (is_would_block(code)) {
      return AcceptOutcome::TimedOut;
    }
    error = "select() failed: " + socket_error_text(code);
    return AcceptOutcome::Closed;
  }
  return AcceptOutcome::Accepted;
}

Status Socket::close() {
  if (!valid()) {
    return ok_status();
  }
#if defined(_WIN32)
  (void)::closesocket(static_cast<SOCKET>(handle_));
#else
  (void)::close(static_cast<int>(handle_));
#endif
  handle_ = kInvalid;
  return ok_status();
}

Status Socket::shutdown_send() {
  if (!valid()) {
    return ok_status();
  }
#if defined(_WIN32)
  if (::shutdown(static_cast<SOCKET>(handle_), SD_SEND) != 0) {
    return fail(ErrorCode::TransportError, "shutdown() failed");
  }
#else
  if (::shutdown(static_cast<int>(handle_), SHUT_WR) != 0) {
    return fail(ErrorCode::TransportError, "shutdown() failed");
  }
#endif
  return ok_status();
}

void Socket::set_nodelay(bool enabled) {
  if (!valid()) {
    return;
  }
  const int value = enabled ? 1 : 0;
  (void)::setsockopt(static_cast<Native>(handle_), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
}

void Socket::set_keepalive(bool enabled) {
  if (!valid()) {
    return;
  }
  const int value = enabled ? 1 : 0;
  (void)::setsockopt(static_cast<Native>(handle_), SOL_SOCKET, SO_KEEPALIVE,
                     reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
}

std::uint16_t Socket::local_port() const {
  if (!valid()) {
    return 0;
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(actual));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(actual));
#endif
  if (::getsockname(static_cast<Native>(handle_), reinterpret_cast<sockaddr*>(&actual), &length) != 0) {
    return 0;
  }
  return ntohs(actual.sin_port);
}

void socket_sleep_millis(int millis) {
  if (millis <= 0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

}  // namespace drain
