#pragma once

// Drain Fabric -- error taxonomy and Result carrier.
//
// Every fallible operation returns a Result. Errors carry a stable ErrorCode so
// that callers (including the CLI and the wire protocol) can branch on the
// failure class deterministically instead of matching on message text.

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "drain/export.hpp"

namespace drain {

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  MalformedIdentity = 2,
  NotFound = 3,
  AlreadyExists = 4,
  DuplicateIdentity = 5,
  StaleGeneration = 6,
  StaleAuthority = 7,
  StaleEpoch = 8,
  StaleIncarnation = 9,
  StaleEvidence = 10,
  StaleRevision = 11,
  AdmissionClosed = 12,
  IllegalTransition = 13,
  PolicyViolation = 14,
  RedundancyViolation = 15,
  CapacityViolation = 16,
  DomainLimitViolation = 17,
  ProtectedObligationRemains = 18,
  AuthorityRequired = 19,
  PersistenceCorrupt = 20,
  PersistenceIo = 21,
  BoundsExceeded = 22,
  Cancelled = 23,
  Unsupported = 24,
  ProtocolError = 25,
  TransportError = 26,
  Busy = 27,
  ShuttingDown = 28,
  Internal = 29,
  Blocked = 30,
};

DRAIN_API const char* to_string(ErrorCode code);

/// True when the error means "the caller presented stale fencing state". The
/// engine never applies a mutation for these codes; callers must re-read
/// authoritative state before retrying.
DRAIN_API bool is_stale_fence(ErrorCode code);

/// True when retrying the identical operation later could plausibly succeed
/// without changing the request.
DRAIN_API bool is_retryable(ErrorCode code);

class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  ErrorCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }
  std::string to_string() const;

  friend bool operator==(const Error& a, const Error& b) noexcept {
    return a.code_ == b.code_;
  }

 private:
  ErrorCode code_{ErrorCode::Ok};
  std::string message_{};
};

template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) {}

  bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const noexcept { return ok(); }

  const T& value() const { return std::get<T>(storage_); }
  T& value() { return std::get<T>(storage_); }
  const T& operator*() const { return std::get<T>(storage_); }
  T& operator*() { return std::get<T>(storage_); }
  const T* operator->() const { return &std::get<T>(storage_); }
  T* operator->() { return &std::get<T>(storage_); }

  /// The error carried by this result. Returns an empty error rather than
  /// throwing when the result is a success, so that diagnostic paths (test
  /// reporters, logging, CLI output) can never fault on a successful result.
  const Error& error() const {
    static const Error kNoError{};
    const Error* found = std::get_if<Error>(&storage_);
    return found == nullptr ? kNoError : *found;
  }
  ErrorCode code() const { return ok() ? ErrorCode::Ok : error().code(); }

  std::optional<T> take() {
    if (!ok()) {
      return std::nullopt;
    }
    return std::optional<T>(std::move(std::get<T>(storage_)));
  }

 private:
  std::variant<T, Error> storage_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)), ok_(false) {}

  static Result success() { return Result(); }

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  const Error& error() const { return error_; }
  ErrorCode code() const { return ok_ ? ErrorCode::Ok : error_.code(); }

 private:
  Error error_{};
  bool ok_{true};
};

using Status = Result<void>;

inline Status ok_status() { return Status(); }

inline Status fail(ErrorCode code, std::string message) {
  return Status(Error(code, std::move(message)));
}

}  // namespace drain
