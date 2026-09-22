#pragma once

// Drain Fabric -- clock abstraction.
//
// Policy deadlines are always evaluated against an injected clock. Tests drive a
// ManualClock so that deadline behaviour is deterministic and reproducible; the
// daemon uses the system clock. No component in this repository measures elapsed
// wall time to decide that something "timed out".

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "drain/export.hpp"

namespace drain {

using Duration = std::chrono::nanoseconds;
using TimePoint = std::chrono::nanoseconds;

inline constexpr Duration kZeroDuration{0};

class Clock {
 public:
  virtual ~Clock();
  virtual TimePoint now() const = 0;
};

class DRAIN_API SystemClock final : public Clock {
 public:
  TimePoint now() const override;
};

/// Deterministic, manually advanced clock. Thread safe: advancing from a
/// controlling thread while the engine reads is well defined.
class DRAIN_API ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(TimePoint start) : nanos_(start.count()) {}

  TimePoint now() const override { return TimePoint(nanos_.load(std::memory_order_acquire)); }

  void advance(Duration delta) {
    nanos_.fetch_add(delta.count(), std::memory_order_acq_rel);
  }

  void set(TimePoint value) { nanos_.store(value.count(), std::memory_order_release); }

 private:
  std::atomic<std::int64_t> nanos_{0};
};

DRAIN_API Clock& system_clock();

/// Deterministic textual rendering of a duration: "<n>ns" plus a millisecond
/// form when the duration is at least a millisecond. No locale, no floating
/// point: explanations must be byte-identical across runs and platforms.
DRAIN_API std::string describe_duration(Duration duration);

/// Deterministic textual rendering of an absolute time point.
DRAIN_API std::string describe_time(TimePoint point);

/// Saturating addition for externally derived durations; prevents overflow from
/// malformed policy input.
DRAIN_API TimePoint saturating_add(TimePoint base, Duration delta) noexcept;

}  // namespace drain
