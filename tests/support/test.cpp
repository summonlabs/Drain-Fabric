#include "test.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace drain_test {

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(std::string suite, std::string name, std::function<void()> body) {
  TestCase test;
  test.suite = std::move(suite);
  test.name = std::move(name);
  test.body = std::move(body);
  cases_.push_back(std::move(test));
}

void fail(const char* file, int line, const std::string& message) {
  std::string text = file;
  text.append(":");
  text.append(std::to_string(line));
  text.append(": ");
  text.append(message);
  throw Failure(text);
}

namespace {

struct Options {
  std::string filter{};
  std::string suite{};
  bool list{false};
  bool verbose{false};
};

Options parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      options.list = true;
    } else if (argument == "--verbose") {
      options.verbose = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      options.filter = argument.substr(9);
    } else if (argument.rfind("--suite=", 0) == 0) {
      options.suite = argument.substr(8);
    }
  }
  return options;
}

bool matches(const Options& options, const TestCase& test) {
  if (!options.suite.empty() && test.suite != options.suite) {
    return false;
  }
  if (options.filter.empty()) {
    return true;
  }
  return test.name.find(options.filter) != std::string::npos ||
         test.suite.find(options.filter) != std::string::npos;
}

}  // namespace

int run_all(int argc, char** argv) {
  const Options options = parse(argc, argv);
  const auto& cases = Registry::instance().cases();
  if (options.list) {
    for (const auto& test : cases) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  for (const auto& test : cases) {
    if (!matches(options, test)) {
      continue;
    }
    const std::string label = test.suite + "." + test.name;
    try {
      test.body();
      ++passed;
      if (options.verbose) {
        std::printf("PASS %s\n", label.c_str());
      }
    } catch (const Failure& failure) {
      ++failed;
      failures.push_back(label + "\n    " + failure.message());
      std::printf("FAIL %s\n    %s\n", label.c_str(), failure.message().c_str());
    } catch (const std::exception& error) {
      ++failed;
      failures.push_back(label + "\n    unexpected exception: " + error.what());
      std::printf("FAIL %s\n    unexpected exception: %s\n", label.c_str(), error.what());
    } catch (...) {
      ++failed;
      failures.push_back(label + "\n    unexpected non-standard exception");
      std::printf("FAIL %s\n    unexpected non-standard exception\n", label.c_str());
    }
  }

  std::printf("\n%zu passed, %zu failed\n", passed, failed);
  if (!failures.empty()) {
    std::printf("failures:\n");
    for (const auto& failure : failures) {
      std::printf("  %s\n", failure.c_str());
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace drain_test

int main(int argc, char** argv) { return ::drain_test::run_all(argc, argv); }
