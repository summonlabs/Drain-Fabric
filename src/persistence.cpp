#include "drain/persistence.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>

#include "drain/checked.hpp"
#include "drain/crc32c.hpp"
#include "drain/version.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace drain {
namespace {

constexpr std::array<unsigned char, 8> kMagic{'D', 'R', 'N', 'F', 'A', 'B', 0x00, 0x01};
constexpr std::uint32_t kReservedMarker = 0xffffffffu;

void put_u16(std::vector<unsigned char>& out, std::uint16_t value) {
  out.push_back(static_cast<unsigned char>(value & 0xffu));
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xffu));
}

void put_u32(std::vector<unsigned char>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<unsigned char>((value >> shift) & 0xffu));
  }
}

void put_u64(std::vector<unsigned char>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<unsigned char>((value >> shift) & 0xffu));
  }
}

std::uint16_t get_u16(const unsigned char* data) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t get_u32(const unsigned char* data) {
  std::uint32_t value = 0;
  for (int index = 3; index >= 0; --index) {
    value = static_cast<std::uint32_t>((value << 8) | data[index]);
  }
  return value;
}

std::uint64_t get_u64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

Status read_whole_file(const std::filesystem::path& file, std::uint64_t max_bytes,
                       std::vector<unsigned char>& out) {
  std::error_code error;
  const bool present = std::filesystem::exists(file, error);
  if (error) {
    return fail(ErrorCode::PersistenceIo, "cannot stat " + file.string() + ": " + error.message());
  }
  if (!present) {
    return fail(ErrorCode::NotFound, "no snapshot at " + file.string());
  }
  const std::uintmax_t size = std::filesystem::file_size(file, error);
  if (error) {
    return fail(ErrorCode::PersistenceIo, "cannot size " + file.string() + ": " + error.message());
  }
  if (size > max_bytes) {
    return fail(ErrorCode::BoundsExceeded, "snapshot file exceeds the configured bound");
  }
  const auto narrowed = narrow_checked<std::size_t>(size);
  if (!narrowed.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "snapshot file size does not fit the platform size type");
  }
  std::FILE* handle = std::fopen(file.string().c_str(), "rb");
  if (handle == nullptr) {
    return fail(ErrorCode::PersistenceIo, "cannot open " + file.string());
  }
  out.resize(*narrowed);
  std::size_t read = 0;
  if (*narrowed > 0) {
    read = std::fread(out.data(), 1, *narrowed, handle);
  }
  const bool short_read = read != *narrowed;
  std::fclose(handle);
  if (short_read) {
    return fail(ErrorCode::PersistenceIo, "short read from " + file.string());
  }
  return ok_status();
}

Status write_whole_file(const std::filesystem::path& file, const std::vector<unsigned char>& bytes) {
  std::FILE* handle = std::fopen(file.string().c_str(), "wb");
  if (handle == nullptr) {
    return fail(ErrorCode::PersistenceIo, "cannot create " + file.string());
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), handle);
    if (written != bytes.size()) {
      std::fclose(handle);
      return fail(ErrorCode::PersistenceIo, "short write to " + file.string());
    }
  }
  if (std::fflush(handle) != 0) {
    std::fclose(handle);
    return fail(ErrorCode::PersistenceIo, "flush failed for " + file.string());
  }
#if defined(_WIN32)
  const int descriptor = _fileno(handle);
  if (descriptor >= 0 && _commit(descriptor) != 0) {
    std::fclose(handle);
    return fail(ErrorCode::PersistenceIo, "commit failed for " + file.string());
  }
#else
  const int descriptor = fileno(handle);
  if (descriptor >= 0 && fsync(descriptor) != 0) {
    std::fclose(handle);
    return fail(ErrorCode::PersistenceIo, "fsync failed for " + file.string());
  }
#endif
  if (std::fclose(handle) != 0) {
    return fail(ErrorCode::PersistenceIo, "close failed for " + file.string());
  }
  return ok_status();
}

}  // namespace

Persistence::Persistence(std::filesystem::path path, PersistenceLimits limits)
    : path_(std::move(path)), limits_(limits) {}

std::filesystem::path Persistence::backup_path() const {
  std::filesystem::path backup = path_;
  backup += ".bak";
  return backup;
}

std::filesystem::path Persistence::temporary_path() const {
  std::filesystem::path temporary = path_;
  temporary += ".tmp";
  return temporary;
}

bool Persistence::exists() const {
  std::error_code error;
  return std::filesystem::exists(path_, error) && !error;
}

Status Persistence::save(const JsonValue& snapshot, IncarnationId incarnation, TimePoint now) {
  (void)now;
  const std::string payload = snapshot.dump(0);
  if (payload.size() > limits_.max_snapshot_bytes) {
    return fail(ErrorCode::BoundsExceeded, "snapshot payload exceeds the configured bound");
  }
  const auto next_sequence = increment_checked(written_sequence_);
  if (!next_sequence.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "snapshot sequence space exhausted");
  }

  std::vector<unsigned char> bytes;
  bytes.reserve(kSnapshotOverheadBytes + payload.size());
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  put_u16(bytes, kSnapshotFormatVersion);
  put_u16(bytes, 0);
  put_u32(bytes, crc32c(bytes.data(), bytes.size()));
  put_u64(bytes, *next_sequence);
  put_u64(bytes, incarnation.value());
  put_u64(bytes, static_cast<std::uint64_t>(payload.size()));
  put_u32(bytes, crc32c(payload.data(), payload.size()));
  put_u32(bytes, kReservedMarker);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  put_u32(bytes, crc32c(bytes.data(), bytes.size()));

  std::error_code error;
  const std::filesystem::path temporary = temporary_path();
  std::filesystem::remove(temporary, error);
  error.clear();
  Status written = write_whole_file(temporary, bytes);
  if (!written.ok()) {
    return written;
  }
  if (exists()) {
    const std::filesystem::path backup = backup_path();
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::copy_file(path_, backup, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      return fail(ErrorCode::PersistenceIo, "cannot rotate the previous snapshot: " + error.message());
    }
  }
  std::filesystem::rename(temporary, path_, error);
  if (error) {
    // Some platforms cannot replace an existing file with rename.
    std::filesystem::remove(path_, error);
    error.clear();
    std::filesystem::rename(temporary, path_, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      return fail(ErrorCode::PersistenceIo, "cannot install the new snapshot: " + error.message());
    }
  }
  written_sequence_ = *next_sequence;
  return ok_status();
}

Result<JsonValue> Persistence::read_file(const std::filesystem::path& file, bool from_backup,
                                        SnapshotInfo& info) const {
  const std::uint64_t bound = limits_.max_snapshot_bytes + kSnapshotOverheadBytes;
  std::vector<unsigned char> bytes;
  Status read = read_whole_file(file, bound, bytes);
  if (!read.ok()) {
    return read.error();
  }
  if (bytes.size() < kSnapshotOverheadBytes) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot is shorter than its fixed overhead");
  }
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot magic does not match");
  }
  if (crc32c(bytes.data(), 12) != get_u32(bytes.data() + 12)) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot header checksum does not match");
  }
  const std::uint16_t format = get_u16(bytes.data() + 8);
  if (format != kSnapshotFormatVersion) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot format version is not supported");
  }
  if (get_u16(bytes.data() + 10) != 0) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot header flags must be zero");
  }
  if (get_u32(bytes.data() + 44) != kReservedMarker) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot reserved marker is wrong");
  }
  const std::uint64_t sequence = get_u64(bytes.data() + 16);
  const std::uint64_t incarnation = get_u64(bytes.data() + 24);
  const std::uint64_t payload_length = get_u64(bytes.data() + 32);
  if (payload_length > limits_.max_snapshot_bytes) {
    return Error(ErrorCode::BoundsExceeded, "snapshot payload length exceeds the configured bound");
  }
  const auto expected_total = add_checked<std::uint64_t>(kSnapshotOverheadBytes, payload_length);
  if (!expected_total.has_value() || *expected_total != static_cast<std::uint64_t>(bytes.size())) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot length field does not match the file size");
  }
  const std::uint32_t payload_crc = get_u32(bytes.data() + 40);
  const auto* payload = bytes.data() + kSnapshotHeaderBytes;
  if (crc32c(payload, static_cast<std::size_t>(payload_length)) != payload_crc) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot payload checksum does not match");
  }
  const std::size_t footer_offset = kSnapshotHeaderBytes + static_cast<std::size_t>(payload_length);
  if (crc32c(bytes.data(), footer_offset) != get_u32(bytes.data() + footer_offset)) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot file checksum does not match");
  }

  const std::string_view text(reinterpret_cast<const char*>(payload),
                              static_cast<std::size_t>(payload_length));
  JsonLimits json_limits;
  json_limits.max_total_bytes = static_cast<std::size_t>(limits_.max_snapshot_bytes);
  auto parsed = json_parse(text, json_limits);
  if (!parsed.ok()) {
    return Error(ErrorCode::PersistenceCorrupt, "snapshot payload is not valid json: " +
                                                    parsed.error().message());
  }

  info.format_version = format;
  info.sequence = sequence;
  info.incarnation = IncarnationId{incarnation};
  info.payload_bytes = payload_length;
  info.payload_crc = payload_crc;
  info.from_backup = from_backup;
  info.source = file.string();
  return *parsed;
}

Result<JsonValue> Persistence::load(SnapshotInfo& info) const {
  SnapshotInfo primary;
  auto primary_result = read_file(path_, false, primary);
  if (primary_result.ok()) {
    info = primary;
    return *primary_result;
  }
  if (primary_result.error().code() == ErrorCode::NotFound) {
    return primary_result.error();
  }
  const Error primary_error = primary_result.error();
  SnapshotInfo backup;
  auto backup_result = read_file(backup_path(), true, backup);
  if (!backup_result.ok()) {
    return Error(primary_error.code(), "live snapshot rejected (" + primary_error.message() +
                                           ") and the backup is unusable (" +
                                           backup_result.error().message() + ")");
  }
  info = backup;
  return *backup_result;
}

Status Persistence::verify(SnapshotInfo& info) const {
  SnapshotInfo local;
  auto loaded = read_file(path_, false, local);
  if (!loaded.ok()) {
    SnapshotInfo backup_info;
    auto backup = read_file(backup_path(), true, backup_info);
    if (!backup.ok()) {
      return loaded.error();
    }
    info = backup_info;
    return ok_status();
  }
  info = local;
  return ok_status();
}

Status Persistence::reset() {
  std::error_code error;
  std::filesystem::remove(path_, error);
  error.clear();
  std::filesystem::remove(backup_path(), error);
  error.clear();
  std::filesystem::remove(temporary_path(), error);
  written_sequence_ = 0;
  return ok_status();
}

}  // namespace drain
