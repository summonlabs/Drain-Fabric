#pragma once

// Drain Fabric -- runtime facade.
//
// The runtime owns the boot sequence: it reads the boot marker and the last
// snapshot, chooses a strictly-increasing incarnation, installs the incarnation
// fence, and only then allows the engine to be used. Nothing recovered from disk
// becomes authoritative merely because it deserialized: evidence stays
// quarantined and every transient drain state is revalidated.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "drain/clock.hpp"
#include "drain/engine.hpp"
#include "drain/export.hpp"
#include "drain/persistence.hpp"
#include "drain/policy.hpp"
#include "drain/result.hpp"

namespace drain {

struct RuntimeOptions {
  DrainPolicy policy{};
  /// Empty means the runtime keeps state in memory only.
  std::filesystem::path state_path{};
  /// Externally supplied monotonic boot counter. The runtime never chooses an
  /// incarnation at or below one it has already observed.
  std::uint64_t boot_id{0};
  PersistenceLimits persistence_limits{};
  /// When true the configured policy replaces the snapshot policy at start.
  bool override_policy{false};
  /// When false the runtime starts with empty state even if a snapshot exists.
  bool load_on_start{true};
};

class Runtime {
 public:
  Runtime(RuntimeOptions options, Clock& clock);
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  /// Recovers state and installs the incarnation. Idempotent.
  Status start(TimePoint now);

  /// Advances the engine and, when persistent, writes a snapshot.
  Status tick(TimePoint now);

  /// Writes a snapshot without advancing.
  Status save(TimePoint now);

  /// Revokes authority and writes a final snapshot.
  Status stop(TimePoint now);

  Status reset_state();

  DrainEngine& engine() noexcept { return engine_; }
  const DrainEngine& engine() const noexcept { return engine_; }

  IncarnationId incarnation() const noexcept { return incarnation_; }
  std::uint64_t boot_id() const noexcept { return boot_id_; }
  bool persistent() const noexcept { return persistence_ != nullptr; }
  bool started() const noexcept { return started_; }
  const Persistence* persistence() const noexcept { return persistence_.get(); }
  const RuntimeOptions& options() const noexcept { return options_; }

  /// The authority line recorded by the previous boot, or zeros when there is
  /// no marker yet.
  struct BootRecord {
    std::uint64_t incarnation{0};
    std::uint64_t epoch{0};
  };

  /// Reads the recorded boot line without starting.
  Result<BootRecord> peek_boot_record() const;

 private:
  Status write_boot_marker(const BootRecord& record);

  RuntimeOptions options_;
  Clock& clock_;
  DrainEngine engine_;
  std::unique_ptr<Persistence> persistence_{};
  IncarnationId incarnation_{};
  std::uint64_t boot_id_{0};
  bool started_{false};
};

}  // namespace drain
