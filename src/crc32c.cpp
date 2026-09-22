#include "drain/crc32c.hpp"

#include <array>

namespace drain {
namespace {

constexpr std::uint32_t kPolynomial = 0x82f63b78u;  // reflected CRC-32C polynomial

struct Table {
  std::array<std::uint32_t, 256> entries{};
  constexpr Table() {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1) ^ kPolynomial : (crc >> 1);
      }
      entries[i] = crc;
    }
  }
};

constexpr Table kTable{};

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t length, std::uint32_t seed) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < length; ++i) {
    crc = kTable.entries[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace drain
