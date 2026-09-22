#pragma once

// Drain Fabric -- child process support for the independent-process tests.
//
// The tests that claim distributed behaviour must run real OS processes over
// real sockets. This helper starts a program with its standard streams
// redirected to a file (never a pipe), waits for it to exit, and guarantees that
// a child is terminated when the helper is destroyed so a failing test can never
// leave an orphan behind.

#include <cstdint>
#include <string>
#include <vector>

#include "drain/result.hpp"

namespace drain_test {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Starts a program. Standard output and standard error are redirected to
  /// log_path. The working directory may be empty to inherit the parent's.
  static drain::Result<ChildProcess> spawn(const std::string& program,
                                           const std::vector<std::string>& arguments,
                                           const std::string& working_directory,
                                           const std::string& log_path);

  bool running() const;
  /// Blocks until the child exits and returns its exit code.
  int wait();
  /// Forces the child to stop. Safe to call repeatedly.
  void terminate();
  bool terminate_and_wait(int& exit_code);

  std::uint64_t pid() const noexcept { return pid_; }
  const std::string& log_path() const noexcept { return log_path_; }
  std::string read_log() const;

  /// Waits until a file appears. This is process coordination, not a test
  /// watchdog: the caller passes an explicit budget and receives false when it
  /// expires.
  static bool wait_for_file(const std::string& path, int budget_ms, int poll_interval_ms = 5);

 private:
  std::uint64_t pid_{0};
  void* handle_{nullptr};
  std::string log_path_{};
};

/// Quoting helper for building a command line on Windows.
std::string quote_argument(const std::string& argument);

/// Creates (or replaces) a file with the given contents.
bool write_text_file(const std::string& path, const std::string& text);

/// Reads a whole file; returns false when it cannot be read.
bool read_text_file(const std::string& path, std::string& out);

/// Removes a file if it exists.
void remove_file(const std::string& path);

/// A unique temporary directory under the system temp root.
std::string make_temp_directory(const std::string& tag);

}  // namespace drain_test
