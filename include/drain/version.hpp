#pragma once

#include <cstdint>
#include <string>

#include "drain/export.hpp"

namespace drain {

inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;
inline constexpr std::uint16_t kVersionPatch = 0;

/// Persistence format version. Any change to the on-disk snapshot layout must bump this.
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;

/// Wire protocol version negotiated during the agent/controller handshake.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

DRAIN_API std::string version_string();

}  // namespace drain
