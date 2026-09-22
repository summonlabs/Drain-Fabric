#include "drain/digest.hpp"

#include <array>

namespace drain {
namespace {
constexpr std::uint64_t kPrime = 1099511628211ULL;
}

void Digest64::update(const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < length; ++i) {
    state_ ^= static_cast<std::uint64_t>(bytes[i]);
    state_ *= kPrime;
  }
}

void Digest64::update_u64(std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    const auto byte = static_cast<unsigned char>((value >> shift) & 0xffULL);
    state_ ^= static_cast<std::uint64_t>(byte);
    state_ *= kPrime;
  }
}

void Digest64::update_u32(std::uint32_t value) noexcept { update_u64(static_cast<std::uint64_t>(value)); }

void Digest64::update_bool(bool value) noexcept { update_u64(value ? 1ULL : 0ULL); }

void Digest64::update_tagged(std::string_view tag, std::string_view value) noexcept {
  update_u64(tag.size());
  update(tag);
  update_u64(value.size());
  update(value);
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  Digest64 digest;
  digest.update(text);
  return digest.value();
}

std::string hex_u32(std::uint32_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out(8, '0');
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[value & 0xfu];
    value >>= 4;
  }
  return out;
}

std::string hex_u64(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[value & 0xfULL];
    value >>= 4;
  }
  return out;
}

}  // namespace drain
