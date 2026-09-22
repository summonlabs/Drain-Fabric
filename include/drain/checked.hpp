#pragma once

// Drain Fabric -- checked arithmetic for externally derived sizes.
//
// Every quantity that originates outside the runtime (wire frames, snapshot
// payloads, policy files, CLI arguments, obligation capacities) passes through
// these helpers. Overflow is reported, never wrapped. The implementation uses
// only portable integer operations so that it behaves identically under MSVC,
// GCC, and Clang.

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

#include "drain/export.hpp"

namespace drain {

/// Lossless cast to a narrower type. Returns nullopt when the value does not fit.
template <class To, class From>
std::optional<To> narrow_checked(From value) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>, "integral types required");
  if constexpr (std::is_signed_v<From> && std::is_unsigned_v<To>) {
    if (value < From{0}) {
      return std::nullopt;
    }
  }
  using CommonFrom = std::common_type_t<From, To>;
  const auto widened = static_cast<CommonFrom>(value);
  if (widened > static_cast<CommonFrom>(std::numeric_limits<To>::max())) {
    return std::nullopt;
  }
  if constexpr (std::is_signed_v<To>) {
    if (widened < static_cast<CommonFrom>(std::numeric_limits<To>::min())) {
      return std::nullopt;
    }
  }
  return static_cast<To>(value);
}

template <class T>
std::optional<T> add_checked(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral type required");
  using U = std::make_unsigned_t<T>;
  if constexpr (std::is_unsigned_v<T>) {
    const U ua = static_cast<U>(a);
    const U ub = static_cast<U>(b);
    if (ua > static_cast<U>(std::numeric_limits<U>::max() - ub)) {
      return std::nullopt;
    }
    return static_cast<T>(static_cast<U>(ua + ub));
  } else {
    if (b > T{0}) {
      if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
        return std::nullopt;
      }
    } else if (b < T{0}) {
      if (a < static_cast<T>(std::numeric_limits<T>::min() - b)) {
        return std::nullopt;
      }
    }
    return static_cast<T>(static_cast<U>(a) + static_cast<U>(b));
  }
}

template <class T>
std::optional<T> sub_checked(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral type required");
  using U = std::make_unsigned_t<T>;
  if constexpr (std::is_unsigned_v<T>) {
    if (static_cast<U>(b) > static_cast<U>(a)) {
      return std::nullopt;
    }
  } else {
    if (b > T{0}) {
      if (a < static_cast<T>(std::numeric_limits<T>::min() + b)) {
        return std::nullopt;
      }
    } else if (b < T{0}) {
      if (a > static_cast<T>(std::numeric_limits<T>::max() + b)) {
        return std::nullopt;
      }
    }
  }
  return static_cast<T>(static_cast<U>(a) - static_cast<U>(b));
}

template <class T>
std::optional<T> mul_checked(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral type required");
  using U = std::make_unsigned_t<T>;
  const U ua = a < T{0} ? static_cast<U>(U{0} - static_cast<U>(a)) : static_cast<U>(a);
  const U ub = b < T{0} ? static_cast<U>(U{0} - static_cast<U>(b)) : static_cast<U>(b);
  const U limit = static_cast<U>(std::numeric_limits<U>::max());
  if (ua != U{0} && ub > static_cast<U>(limit / ua)) {
    return std::nullopt;
  }
  const U product = static_cast<U>(ua * ub);
  if constexpr (std::is_unsigned_v<T>) {
    return static_cast<T>(product);
  } else {
    const bool negative = (a < T{0}) != (b < T{0});
    const U positive_limit = static_cast<U>(std::numeric_limits<T>::max());
    if (negative) {
      if (product > static_cast<U>(positive_limit + U{1})) {
        return std::nullopt;
      }
      return static_cast<T>(static_cast<U>(U{0} - product));
    }
    if (product > positive_limit) {
      return std::nullopt;
    }
    return static_cast<T>(product);
  }
}

/// Increment with overflow detection; used for every monotonic counter so that a
/// counter can never wrap into a reused identity.
template <class T>
std::optional<T> increment_checked(T value) noexcept {
  return add_checked<T>(value, T{1});
}

/// Multiply-and-divide in a single checked step: computes value * numerator /
/// denominator with 128-bit-free intermediate handling. Used for policy ratios
/// so that capacity comparisons stay in integer arithmetic.
template <class T>
std::optional<T> scale_ratio_checked(T value, T numerator, T denominator) noexcept {
  static_assert(std::is_unsigned_v<T>, "unsigned type required");
  if (denominator == T{0}) {
    return std::nullopt;
  }
  const auto product = mul_checked<T>(value, numerator);
  if (!product.has_value()) {
    return std::nullopt;
  }
  return static_cast<T>(*product / denominator);
}

}  // namespace drain
