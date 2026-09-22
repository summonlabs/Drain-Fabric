#include "process.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace drain_test {
namespace {

void sleep_millis(int millis) { std::this_thread::sleep_for(std::chrono::milliseconds(millis)); }

}  // namespace

std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  for (const char character : argument) {
    if (character == '"') {
      out.append("\\\"");
    } else {
      out.push_back(character);
    }
  }
  out.push_back('"');
  return out;
}

bool write_text_file(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.flush();
  return static_cast<bool>(stream);
}

bool read_text_file(const std::string& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

void remove_file(const std::string& path) { (void)std::remove(path.c_str()); }

std::string make_temp_directory(const std::string& tag) {
#if defined(_WIN32)
  const char* base = std::getenv("TEMP");
  std::string root = base != nullptr ? base : ".";
#else
  const char* base = std::getenv("TMPDIR");
  std::string root = base != nullptr ? base : "/tmp";
#endif
  // The directory name includes the process id so that repeated runs of the
  // same binary never inherit debris from an earlier run.
#if defined(_WIN32)
  const unsigned long process_id = static_cast<unsigned long>(GetCurrentProcessId());
#else
  const unsigned long process_id = static_cast<unsigned long>(getpid());
#endif
  std::string path = root + "/drainfabric-" + tag + "-" + std::to_string(process_id);
#if defined(_WIN32)
  (void)CreateDirectoryA(path.c_str(), nullptr);
#else
  (void)mkdir(path.c_str(), 0700);
#endif
  return path;
}

ChildProcess::~ChildProcess() { terminate(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_), handle_(other.handle_), log_path_(std::move(other.log_path_)) {
  other.pid_ = 0;
  other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    terminate();
    pid_ = other.pid_;
    handle_ = other.handle_;
    log_path_ = std::move(other.log_path_);
    other.pid_ = 0;
    other.handle_ = nullptr;
  }
  return *this;
}

drain::Result<ChildProcess> ChildProcess::spawn(const std::string& program,
                                                const std::vector<std::string>& arguments,
                                                const std::string& working_directory,
                                                const std::string& log_path) {
#if defined(_WIN32)
  std::string command_line = quote_argument(program);
  for (const auto& argument : arguments) {
    command_line.push_back(' ');
    command_line.append(quote_argument(argument));
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE log = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) {
    return drain::Error(drain::ErrorCode::PersistenceIo, "cannot create the child log file");
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = log;
  startup.hStdError = log;
  startup.hStdInput = nullptr;
  PROCESS_INFORMATION process{};
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');
  const char* directory = working_directory.empty() ? nullptr : working_directory.c_str();
  const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, directory, &startup, &process);
  CloseHandle(log);
  if (created == FALSE) {
    return drain::Error(drain::ErrorCode::TransportError,
                        "CreateProcess failed with " + std::to_string(GetLastError()));
  }
  CloseHandle(process.hThread);
  ChildProcess child;
  child.pid_ = static_cast<std::uint64_t>(process.dwProcessId);
  child.handle_ = process.hProcess;
  child.log_path_ = log_path;
  return child;
#else
  std::string command = program;
  for (const auto& argument : arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
  command.append(" > ");
  command.append(quote_argument(log_path));
  command.append(" 2>&1");
  const pid_t pid = fork();
  if (pid < 0) {
    return drain::Error(drain::ErrorCode::TransportError, "fork failed");
  }
  if (pid == 0) {
    if (!working_directory.empty()) {
      (void)chdir(working_directory.c_str());
    }
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ChildProcess child;
  child.pid_ = static_cast<std::uint64_t>(pid);
  child.log_path_ = log_path;
  return child;
#endif
}

bool ChildProcess::running() const {
#if defined(_WIN32)
  if (handle_ == nullptr) {
    return false;
  }
  return WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_TIMEOUT;
#else
  if (pid_ == 0) {
    return false;
  }
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  return result == 0;
#endif
}

int ChildProcess::wait() {
#if defined(_WIN32)
  if (handle_ == nullptr) {
    return -1;
  }
  (void)WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  (void)GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
  CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  pid_ = 0;
  return static_cast<int>(code);
#else
  if (pid_ == 0) {
    return -1;
  }
  int status = 0;
  (void)waitpid(static_cast<pid_t>(pid_), &status, 0);
  pid_ = 0;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void ChildProcess::terminate() {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    (void)TerminateProcess(static_cast<HANDLE>(handle_), 1);
    (void)WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    pid_ = 0;
  }
#else
  if (pid_ != 0) {
    (void)kill(static_cast<pid_t>(pid_), SIGKILL);
    int status = 0;
    (void)waitpid(static_cast<pid_t>(pid_), &status, 0);
    pid_ = 0;
  }
#endif
}

bool ChildProcess::terminate_and_wait(int& exit_code) {
  if (!running()) {
    exit_code = wait();
    return true;
  }
  terminate();
  exit_code = -1;
  return false;
}

std::string ChildProcess::read_log() const {
  std::string text;
  (void)read_text_file(log_path_, text);
  return text;
}

bool ChildProcess::wait_for_file(const std::string& path, int budget_ms, int poll_interval_ms) {
  int waited = 0;
  for (;;) {
    std::ifstream stream(path, std::ios::binary);
    if (stream) {
      return true;
    }
    if (waited >= budget_ms) {
      return false;
    }
    sleep_millis(poll_interval_ms);
    waited += poll_interval_ms;
  }
}

}  // namespace drain_test
