#pragma once

// Drain Fabric -- stable content digests.
//
// Digests are used for authoritative-snapshot identity, completion proofs, and
// policy fingerprints. FNV-1a/64 is used because it is trivially stable across
// platforms and runs, which matters when a digest is compared against a value
// recorded in a previous incarnation. It is an integrity fingerprint, not a
// cryptographic commitment.

#include <cstdint>
#include <string>
#include <string_view>

#include "drain/export.hpp"

namespace drain {

class Digest64 {
 public:
  Digest64() = default;

  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u64(std::uint64_t value) noexcept;
  void update_u32(std::uint32_t value) noexcept;
  void update_bool(bool value) noexcept;
  void update_tagged(std::string_view tag, std::string_view value) noexcept;

  std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_{1469598103934665603ULL};
};

DRAIN_API std::uint64_t fnv1a64(std::string_view text) noexcept;

/// Lower-case hexadecimal rendering, zero padded to 16 digits.
DRAIN_API std::string hex_u64(std::uint64_t value);

/// Lower-case hexadecimal rendering, zero padded to 8 digits.
DRAIN_API std::string hex_u32(std::uint32_t value);

}  // namespace drain
