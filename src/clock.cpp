#include "drain/clock.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

namespace drain {

Clock::~Clock() = default;

TimePoint SystemClock::now() const {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<Duration>(since);
}

Clock& system_clock() {
  static SystemClock instance;
  return instance;
}

std::string describe_duration(Duration duration) {
  const std::int64_t nanos = duration.count();
  std::string out = std::to_string(nanos);
  out.append("ns");
  if (nanos >= 1000000 || nanos <= -1000000) {
    const std::int64_t negative = nanos < 0 ? -1 : 1;
    const std::uint64_t magnitude = static_cast<std::uint64_t>(nanos < 0 ? -nanos : nanos);
    const std::uint64_t millis = magnitude / 1000000ULL;
    const std::uint64_t micros = (magnitude % 1000000ULL) / 1000ULL;
    out.append(" (");
    if (negative < 0) {
      out.push_back('-');
    }
    out.append(std::to_string(millis));
    out.push_back('.');
    std::string fraction = std::to_string(micros);
    out.append(std::string(3 - std::min<std::size_t>(3, fraction.size()), '0'));
    out.append(fraction);
    out.append("ms)");
  }
  return out;
}

std::string describe_time(TimePoint point) {
  return std::to_string(point.count()) + "ns";
}

TimePoint saturating_add(TimePoint base, Duration delta) noexcept {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  const std::int64_t a = base.count();
  const std::int64_t b = delta.count();
  if (b > 0 && a > kMax - b) {
    return TimePoint(kMax);
  }
  if (b < 0 && a < kMin - b) {
    return TimePoint(kMin);
  }
  return TimePoint(a + b);
}

}  // namespace drain
