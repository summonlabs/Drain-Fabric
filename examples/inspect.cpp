// Drain Fabric example -- snapshot inspection.
//
// Writes a snapshot with the engine, verifies its integrity envelope, and
// reports what the recovery path would accept. Useful as a starting point for
// operators who need to look at Drain Fabric state without a running daemon.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "drain/persistence.hpp"
#include "drain/runtime.hpp"

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : std::string("inspect-state.drainlog");
  drain::SystemClock clock;

  drain::RuntimeOptions options;
  options.state_path = path;
  drain::Runtime runtime(options, clock);
  drain::Status started = runtime.start(clock.now());
  if (!started.ok()) {
    std::printf("start failed: %s\n", started.error().to_string().c_str());
    return 1;
  }
  drain::Status saved = runtime.save(clock.now());
  if (!saved.ok()) {
    std::printf("save failed: %s\n", saved.error().to_string().c_str());
    return 1;
  }

  const drain::Persistence& persistence = *runtime.persistence();
  drain::SnapshotInfo info;
  drain::Status verified = persistence.verify(info);
  if (!verified.ok()) {
    std::printf("verify failed: %s\n", verified.error().to_string().c_str());
    return 1;
  }
  std::printf("snapshot %s\n", persistence.path().string().c_str());
  std::printf("  format version : %u\n", static_cast<unsigned>(info.format_version));
  std::printf("  sequence       : %llu\n", static_cast<unsigned long long>(info.sequence));
  std::printf("  incarnation    : %llu\n", static_cast<unsigned long long>(info.incarnation.value()));
  std::printf("  payload bytes  : %llu\n", static_cast<unsigned long long>(info.payload_bytes));
  std::printf("  payload crc    : %08x\n", info.payload_crc);
  std::printf("  from backup    : %s\n", info.from_backup ? "yes" : "no");

  drain::SnapshotInfo loaded;
  auto payload = persistence.load(loaded);
  if (!payload.ok()) {
    std::printf("load failed: %s\n", payload.error().to_string().c_str());
    return 1;
  }
  std::printf("  drains in file : %zu\n",
              payload->find("drains") == nullptr ? 0 : payload->find("drains")->size());
  std::printf("  incarnation    : %llu\n",
              static_cast<unsigned long long>(payload->get_uint("incarnation")));
  return 0;
}
