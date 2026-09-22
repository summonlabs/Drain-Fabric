#pragma once

// Drain Fabric -- durable state.
//
// Snapshots are versioned, length-delimited, and integrity checked end to end:
// a header CRC over the fixed header, a payload CRC over the bytes, and a file
// CRC over everything before it. A truncated, reordered, or corrupted file is
// rejected rather than partially applied, and recovery falls back to the
// previous good snapshot exactly once.
//
// Writing is atomic: the new snapshot is written to a temporary file, flushed
// to stable storage, and only then moved over the live path. The previous
// snapshot is retained as a single backup so persistence growth is bounded.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "drain/clock.hpp"
#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"

namespace drain {

/// Fixed header layout:
///   0  magic        8 bytes  "DRNFAB\x00\x01"
///   8  format       u16      snapshot format version
///  10  header_flags u16      reserved, must be zero
///  12  header_crc   u32      CRC-32C over bytes [0, 12)
///  16  sequence     u64      snapshot sequence, strictly increasing
///  24  incarnation  u64      incarnation that wrote the snapshot
///  32  payload_len  u64      payload length in bytes
///  40  payload_crc  u32      CRC-32C over the payload bytes
///  44  reserved     u32      must be 0xffffffff
///  48  payload      payload_len bytes of UTF-8 JSON
///  ..  file_crc     u32      CRC-32C over bytes [0, 48 + payload_len)
inline constexpr std::size_t kSnapshotHeaderBytes = 48;
inline constexpr std::size_t kSnapshotFooterBytes = 4;
inline constexpr std::size_t kSnapshotOverheadBytes = kSnapshotHeaderBytes + kSnapshotFooterBytes;

struct PersistenceLimits {
  std::uint64_t max_snapshot_bytes = 32ull * 1024ull * 1024ull;
  /// Additional bytes tolerated on disk beyond the live file (the backup).
  std::uint64_t max_extra_bytes = 32ull * 1024ull * 1024ull;
};

/// Metadata describing a snapshot that was read from disk.
struct SnapshotInfo {
  std::uint16_t format_version{0};
  std::uint64_t sequence{0};
  IncarnationId incarnation{};
  std::uint64_t payload_bytes{0};
  std::uint32_t payload_crc{0};
  bool from_backup{false};
  std::string source{};
};

class Persistence {
 public:
  explicit Persistence(std::filesystem::path path, PersistenceLimits limits = PersistenceLimits{});

  /// Serialises and atomically replaces the snapshot file. Rejects payloads
  /// larger than the configured bound without touching the live file.
  Status save(const JsonValue& snapshot, IncarnationId incarnation, TimePoint now);

  /// Reads and fully validates the snapshot. Returns the parsed payload.
  Result<JsonValue> load(SnapshotInfo& info) const;

  /// Validates the file without returning the payload. Used by the CLI verifier
  /// and by the tests.
  Status verify(SnapshotInfo& info) const;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path backup_path() const;
  std::filesystem::path temporary_path() const;

  /// Highest sequence successfully written by this object (not read from disk).
  std::uint64_t written_sequence() const noexcept { return written_sequence_; }

  /// Removes the live snapshot, the backup, and any temporary file.
  Status reset();

  bool exists() const;

 private:
  Result<JsonValue> read_file(const std::filesystem::path& file, bool from_backup, SnapshotInfo& info) const;

  std::filesystem::path path_;
  PersistenceLimits limits_{};
  std::uint64_t written_sequence_{0};
};

}  // namespace drain
