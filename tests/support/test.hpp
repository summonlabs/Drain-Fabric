#pragma once

// Drain Fabric -- test framework.
//
// Deliberately small: tests are plain functions registered at static
// initialisation time, assertions throw, and the runner reports every failure
// without aborting the process. There are no timeouts anywhere in this
// framework; a test that hangs is a defect to diagnose.

#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace drain_test {

class Failure {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  const std::string& message() const noexcept { return message_; }

 private:
  std::string message_;
};

struct TestCase {
  std::string suite{};
  std::string name{};
  std::function<void()> body{};
};

class Registry {
 public:
  static Registry& instance();
  void add(std::string suite, std::string name, std::function<void()> body);
  const std::vector<TestCase>& cases() const noexcept { return cases_; }

 private:
  std::vector<TestCase> cases_{};
};

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    Registry::instance().add(suite, name, std::move(body));
  }
};

namespace detail {

template <class T, class = void>
struct IsStreamable : std::false_type {};

template <class T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

}  // namespace detail

/// Renders a value into a diagnostic string. Enumeration types render as their
/// numeric value and anything without a stream operator renders as a
/// placeholder, so a CHECK_EQ never fails to compile because of the reporter.
template <class T>
std::string describe(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (detail::IsStreamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return std::string("<") + typeid(T).name() + ">";
  }
}

inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(std::uint8_t value) { return std::to_string(static_cast<unsigned>(value)); }
inline std::string describe(std::int8_t value) { return std::to_string(static_cast<int>(value)); }

[[noreturn]] void fail(const char* file, int line, const std::string& message);

int run_all(int argc, char** argv);

}  // namespace drain_test

#define DRAIN_TEST(suite_name, test_name)                                                    \
  static void suite_name##_##test_name##_body();                                             \
  static ::drain_test::Registrar suite_name##_##test_name##_registrar(                        \
      #suite_name, #test_name, suite_name##_##test_name##_body);                             \
  static void suite_name##_##test_name##_body()

#define DRAIN_FAIL(message) ::drain_test::fail(__FILE__, __LINE__, (message))

#define DRAIN_CHECK(condition)                                                              \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      ::drain_test::fail(__FILE__, __LINE__, std::string("check failed: ") + #condition);    \
    }                                                                                       \
  } while (false)

#define DRAIN_CHECK_MSG(condition, message)                                                  \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      ::drain_test::fail(__FILE__, __LINE__,                                                 \
                         std::string("check failed: ") + #condition + " -- " + (message));    \
    }                                                                                       \
  } while (false)

// The operands are captured by value on purpose. Binding a reference would
// dangle whenever the expression produces a reference into a temporary (for
// example *some_optional_returning_function()), because lifetime extension does
// not propagate through operator*. AddressSanitizer caught exactly that.
#define DRAIN_CHECK_EQ(actual, expected)                                                     \
  do {                                                                                      \
    const auto drain_test_actual = (actual);                                                 \
    const auto drain_test_expected = (expected);                                             \
    if (!(drain_test_actual == drain_test_expected)) {                                       \
      ::drain_test::fail(__FILE__, __LINE__,                                                 \
                         std::string("expected ") + #actual + " == " + #expected +           \
                             "\n    actual:   " + ::drain_test::describe(drain_test_actual) + \
                             "\n    expected: " + ::drain_test::describe(drain_test_expected)); \
    }                                                                                       \
  } while (false)

#define DRAIN_CHECK_OK(expression)                                                           \
  do {                                                                                      \
    const auto drain_test_status = (expression);                                             \
    if (!drain_test_status.ok()) {                                                           \
      ::drain_test::fail(__FILE__, __LINE__,                                                 \
                         std::string("expected success from ") + #expression +              \
                             " but got " + drain_test_status.error().to_string());           \
    }                                                                                       \
  } while (false)

#define DRAIN_CHECK_CODE(expression, expected_code)                                          \
  do {                                                                                      \
    const auto drain_test_status = (expression);                                             \
    if (drain_test_status.code() != (expected_code)) {                                       \
      ::drain_test::fail(__FILE__, __LINE__,                                                 \
                         std::string("expected ") + #expression + " to fail with " +        \
                             ::drain::to_string(expected_code) + " but got " +               \
                             ::drain::to_string(drain_test_status.code()) + " (" +           \
                             drain_test_status.error().message() + ")");                     \
    }                                                                                       \
  } while (false)
