#pragma once

// CRC-32C (Castagnoli) used for persistence integrity and wire framing.
// Software table-driven implementation; no external dependency.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "drain/export.hpp"

namespace drain {

DRAIN_API std::uint32_t crc32c(const void* data, std::size_t length, std::uint32_t seed = 0);

inline std::uint32_t crc32c(std::string_view text, std::uint32_t seed = 0) {
  return crc32c(text.data(), text.size(), seed);
}

}  // namespace drain
