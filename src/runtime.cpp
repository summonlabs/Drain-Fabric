#include "drain/runtime.hpp"

#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#include "drain/checked.hpp"
#include "drain/crc32c.hpp"
#include "drain/digest.hpp"

namespace drain {
namespace {

std::filesystem::path boot_marker_path(const std::filesystem::path& state_path) {
  std::filesystem::path marker = state_path;
  marker += ".boot";
  return marker;
}

constexpr const char* kBootMarkerPrefix = "DRNFBOOT 2";

/// Parses one non-negative decimal value from a marker line.
Result<std::uint64_t> parse_decimal(std::string_view digits) {
  if (digits.empty()) {
    return Error(ErrorCode::PersistenceCorrupt, "boot marker line is empty");
  }
  std::uint64_t value = 0;
  for (const char character : digits) {
    if (character < '0' || character > '9') {
      return Error(ErrorCode::PersistenceCorrupt, "boot marker value is not numeric");
    }
    const auto next = mul_checked<std::uint64_t>(value, 10);
    if (!next.has_value()) {
      return Error(ErrorCode::BoundsExceeded, "boot marker value overflows");
    }
    const auto sum = add_checked<std::uint64_t>(*next, static_cast<std::uint64_t>(character - '0'));
    if (!sum.has_value()) {
      return Error(ErrorCode::BoundsExceeded, "boot marker value overflows");
    }
    value = *sum;
  }
  return value;
}

}  // namespace

Runtime::Runtime(RuntimeOptions options, Clock& clock)
    : options_(std::move(options)),
      clock_(clock),
      engine_(options_.policy, clock) {
  if (!options_.state_path.empty()) {
    persistence_ = std::make_unique<Persistence>(options_.state_path, options_.persistence_limits);
  }
}

Runtime::~Runtime() = default;

Result<Runtime::BootRecord> Runtime::peek_boot_record() const {
  BootRecord record;
  if (options_.state_path.empty()) {
    return record;
  }
  const std::filesystem::path marker = boot_marker_path(options_.state_path);
  std::error_code error;
  if (!std::filesystem::exists(marker, error) || error) {
    return record;
  }
  std::FILE* handle = std::fopen(marker.string().c_str(), "rb");
  if (handle == nullptr) {
    return Error(ErrorCode::PersistenceIo, "cannot read the boot marker");
  }
  std::string text;
  char buffer[192];
  const std::size_t read = std::fread(buffer, 1, sizeof(buffer), handle);
  text.append(buffer, read);
  std::fclose(handle);

  const std::size_t first_newline = text.find('\n');
  if (first_newline == std::string::npos || text.compare(0, first_newline, kBootMarkerPrefix) != 0) {
    return Error(ErrorCode::PersistenceCorrupt, "boot marker has an unexpected prefix");
  }
  const std::size_t second_newline = text.find('\n', first_newline + 1);
  const std::size_t third_newline =
      second_newline == std::string::npos ? std::string::npos : text.find('\n', second_newline + 1);
  const std::size_t fourth_newline =
      third_newline == std::string::npos ? std::string::npos : text.find('\n', third_newline + 1);
  if (fourth_newline == std::string::npos) {
    return Error(ErrorCode::PersistenceCorrupt, "boot marker is truncated");
  }
  const std::string incarnation_digits =
      text.substr(first_newline + 1, second_newline - first_newline - 1);
  const std::string epoch_digits = text.substr(second_newline + 1, third_newline - second_newline - 1);
  auto incarnation = parse_decimal(incarnation_digits);
  if (!incarnation.ok()) {
    return incarnation.error();
  }
  auto epoch = parse_decimal(epoch_digits);
  if (!epoch.ok()) {
    return epoch.error();
  }
  const std::string checksum_text = text.substr(third_newline + 1, fourth_newline - third_newline - 1);
  const std::string body = incarnation_digits + " " + epoch_digits;
  if (checksum_text != hex_u32(crc32c(body.data(), body.size()))) {
    return Error(ErrorCode::PersistenceCorrupt, "boot marker checksum does not match");
  }
  record.incarnation = *incarnation;
  record.epoch = *epoch;
  return record;
}

Status Runtime::write_boot_marker(const BootRecord& record) {
  const std::filesystem::path marker = boot_marker_path(options_.state_path);
  const std::string incarnation_digits = std::to_string(record.incarnation);
  const std::string epoch_digits = std::to_string(record.epoch);
  const std::string body = incarnation_digits + " " + epoch_digits;
  const std::string text = std::string(kBootMarkerPrefix) + "\n" + incarnation_digits + "\n" +
                           epoch_digits + "\n" + hex_u32(crc32c(body.data(), body.size())) + "\n";
  std::FILE* handle = std::fopen(marker.string().c_str(), "wb");
  if (handle == nullptr) {
    return fail(ErrorCode::PersistenceIo, "cannot write the boot marker");
  }
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), handle);
  if (written != text.size() || std::fflush(handle) != 0) {
    std::fclose(handle);
    return fail(ErrorCode::PersistenceIo, "failed to write the boot marker");
  }
  std::fclose(handle);
  return ok_status();
}

Status Runtime::start(TimePoint now) {
  if (started_) {
    return ok_status();
  }
  std::uint64_t highest = options_.boot_id;
  std::uint64_t highest_epoch = 0;
  bool recovered = false;
  if (persistence_ != nullptr) {
    auto marker = peek_boot_record();
    if (!marker.ok()) {
      return marker.error();
    }
    if (marker->incarnation > highest) {
      highest = marker->incarnation;
    }
    if (marker->epoch > highest_epoch) {
      highest_epoch = marker->epoch;
    }
    if (options_.load_on_start && persistence_->exists()) {
      SnapshotInfo info;
      auto loaded = persistence_->load(info);
      if (!loaded.ok()) {
        return loaded.error();
      }
      Status restored = engine_.restore(*loaded, info.incarnation, now);
      if (!restored.ok()) {
        return restored;
      }
      if (info.incarnation.value() > highest) {
        highest = info.incarnation.value();
      }
      if (engine_.authority_epoch().value() > highest_epoch) {
        highest_epoch = engine_.authority_epoch().value();
      }
      recovered = true;
    }
  }
  const auto next = increment_checked(highest);
  if (!next.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "incarnation space exhausted");
  }
  const auto next_epoch = increment_checked(highest_epoch);
  if (!next_epoch.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "authority epoch space exhausted");
  }
  boot_id_ = *next;
  incarnation_ = IncarnationId{boot_id_};

  if (persistence_ != nullptr) {
    BootRecord record;
    record.incarnation = boot_id_;
    record.epoch = *next_epoch;
    Status marker_written = write_boot_marker(record);
    if (!marker_written.ok()) {
      return marker_written;
    }
  }
  // Every start installs a strictly newer authority line. A hard kill can
  // therefore never rewind either the incarnation or the epoch, even when no
  // snapshot was written before the process died.
  Status fenced = engine_.install_authority_line(
      incarnation_, Epoch{*next_epoch}, now, "start: previous incarnation authority is void");
  if (!fenced.ok()) {
    return fenced;
  }
  if (!recovered || options_.override_policy) {
    Status applied = engine_.set_policy(options_.policy, now);
    if (!applied.ok()) {
      return applied;
    }
  }
  started_ = true;
  return ok_status();
}

Status Runtime::tick(TimePoint now) {
  if (!started_) {
    Status started_status = start(now);
    if (!started_status.ok()) {
      return started_status;
    }
  }
  Status advanced = engine_.advance(now);
  Status saved = save(now);
  if (!advanced.ok()) {
    return advanced;
  }
  return saved;
}

Status Runtime::save(TimePoint now) {
  (void)now;
  if (persistence_ == nullptr) {
    return ok_status();
  }
  const JsonValue snapshot = engine_.snapshot();
  return persistence_->save(snapshot, incarnation_, clock_.now());
}

Status Runtime::stop(TimePoint now) {
  for (const auto& token : engine_.authority_tokens()) {
    if (!token.revoked()) {
      Status one = engine_.revoke_authority(token.id(), std::string("runtime shutdown"), now);
      if (!one.ok()) {
        return one;
      }
    }
  }
  Status saved = save(now);
  started_ = false;
  return saved;
}

Status Runtime::reset_state() {
  if (persistence_ != nullptr) {
    Status cleared = persistence_->reset();
    if (!cleared.ok()) {
      return cleared;
    }
    std::error_code error;
    std::filesystem::remove(boot_marker_path(options_.state_path), error);
  }
  return ok_status();
}

}  // namespace drain
