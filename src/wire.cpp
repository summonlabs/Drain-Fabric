#include "drain/wire.hpp"

#include <cstring>

#include "drain/checked.hpp"
#include "drain/crc32c.hpp"

namespace drain {
namespace {

void put_u16(std::vector<unsigned char>& out, std::uint16_t value) {
  out.push_back(static_cast<unsigned char>(value & 0xffu));
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xffu));
}

void put_u32(std::vector<unsigned char>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
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

}  // namespace

const char* to_string(MessageType type) {
  switch (type) {
    case MessageType::Hello: return "hello";
    case MessageType::Welcome: return "welcome";
    case MessageType::Failure: return "failure";
    case MessageType::DrainRequest: return "drain-request";
    case MessageType::DrainResponse: return "drain-response";
    case MessageType::StatusRequest: return "status-request";
    case MessageType::StatusResponse: return "status-response";
    case MessageType::CancelRequest: return "cancel-request";
    case MessageType::CancelResponse: return "cancel-response";
    case MessageType::RestoreRequest: return "restore-request";
    case MessageType::RestoreResponse: return "restore-response";
    case MessageType::ExplainRequest: return "explain-request";
    case MessageType::ExplainResponse: return "explain-response";
    case MessageType::ObligationQuery: return "obligation-query";
    case MessageType::ObligationQueryResponse: return "obligation-query-response";
    case MessageType::ObligationAdmit: return "obligation-admit";
    case MessageType::ObligationAdmitResponse: return "obligation-admit-response";
    case MessageType::ObligationReport: return "obligation-report";
    case MessageType::ObligationReportResponse: return "obligation-report-response";
    case MessageType::EvidenceReport: return "evidence-report";
    case MessageType::EvidenceResponse: return "evidence-response";
    case MessageType::EvacuationRequest: return "evacuation-request";
    case MessageType::EvacuationResponse: return "evacuation-response";
    case MessageType::AdmissionReopen: return "admission-reopen";
    case MessageType::AdmissionReopenResponse: return "admission-reopen-response";
    case MessageType::AuditRequest: return "audit-request";
    case MessageType::AuditResponse: return "audit-response";
    case MessageType::Shutdown: return "shutdown";
    case MessageType::ShutdownResponse: return "shutdown-response";
  }
  return "unknown";
}

std::optional<MessageType> message_type_from_string(std::string_view text) {
  for (std::uint16_t raw = 1; raw <= 41; ++raw) {
    if (raw > 25 && raw < 30) {
      continue;
    }
    const auto candidate = static_cast<MessageType>(raw);
    if (text == drain::to_string(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

const char* to_string(StreamOutcome outcome) {
  switch (outcome) {
    case StreamOutcome::Frame: return "frame";
    case StreamOutcome::TimedOut: return "timed-out";
    case StreamOutcome::Closed: return "closed";
    case StreamOutcome::Failed: return "failed";
    case StreamOutcome::ProtocolError: return "protocol-error";
  }
  return "unknown";
}

JsonValue Frame::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("type", JsonValue(std::string(drain::to_string(type))));
  value.set("sequence", JsonValue(static_cast<std::uint64_t>(sequence)));
  value.set("payload", payload);
  return value;
}

Result<std::vector<unsigned char>> encode_frame(const Frame& frame, std::size_t max_payload_bytes) {
  const std::string payload = frame.payload.dump(0);
  if (payload.size() > max_payload_bytes) {
    return Error(ErrorCode::BoundsExceeded, "frame payload exceeds the configured bound");
  }
  const auto sized = narrow_checked<std::uint32_t>(payload.size());
  if (!sized.has_value()) {
    return Error(ErrorCode::BoundsExceeded, "frame payload does not fit a 32-bit length field");
  }
  std::vector<unsigned char> out;
  out.reserve(kFrameHeaderBytes + payload.size());
  put_u32(out, kFrameMagic);
  put_u16(out, kWireProtocolVersion);
  put_u16(out, static_cast<std::uint16_t>(frame.type));
  put_u32(out, frame.sequence);
  put_u32(out, *sized);
  put_u32(out, crc32c(payload.data(), payload.size()));
  put_u32(out, crc32c(out.data(), out.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Result<Frame> decode_frame(const unsigned char* data, std::size_t length, std::size_t max_payload_bytes) {
  if (length < kFrameHeaderBytes) {
    return Error(ErrorCode::ProtocolError, "frame image is shorter than the header");
  }
  if (get_u32(data) != kFrameMagic) {
    return Error(ErrorCode::ProtocolError, "frame magic does not match");
  }
  if (crc32c(data, 20) != get_u32(data + 20)) {
    return Error(ErrorCode::ProtocolError, "frame header checksum does not match");
  }
  const std::uint16_t version = get_u16(data + 4);
  if (version != kWireProtocolVersion) {
    return Error(ErrorCode::ProtocolError, "wire protocol version mismatch");
  }
  const std::uint32_t payload_length = get_u32(data + 12);
  if (payload_length > max_payload_bytes) {
    return Error(ErrorCode::BoundsExceeded, "frame payload exceeds the configured bound");
  }
  const auto total = add_checked<std::size_t>(kFrameHeaderBytes, static_cast<std::size_t>(payload_length));
  if (!total.has_value() || *total != length) {
    return Error(ErrorCode::ProtocolError, "frame length field does not match the image size");
  }
  const auto* payload = data + kFrameHeaderBytes;
  if (crc32c(payload, static_cast<std::size_t>(payload_length)) != get_u32(data + 16)) {
    return Error(ErrorCode::ProtocolError, "frame payload checksum does not match");
  }
  const std::string_view text(reinterpret_cast<const char*>(payload),
                              static_cast<std::size_t>(payload_length));
  JsonLimits limits;
  limits.max_total_bytes = max_payload_bytes;
  auto parsed = json_parse(text, limits);
  if (!parsed.ok()) {
    return Error(ErrorCode::ProtocolError, "frame payload is not valid json: " + parsed.error().message());
  }
  Frame frame;
  frame.type = static_cast<MessageType>(get_u16(data + 6));
  frame.sequence = get_u32(data + 8);
  frame.payload = *parsed;
  return frame;
}

FrameStream::FrameStream(Socket socket, std::size_t max_payload_bytes)
    : socket_(std::move(socket)), max_payload_bytes_(max_payload_bytes) {}

Status FrameStream::send_frame(const Frame& frame) {
  auto encoded = encode_frame(frame, max_payload_bytes_);
  if (!encoded.ok()) {
    return encoded.error();
  }
  return socket_.send_all(encoded->data(), encoded->size());
}

StreamOutcome FrameStream::receive_frame(Frame& frame, int wait_ms, std::string& error) {
  if (closed_) {
    error = "stream is closed";
    return StreamOutcome::Closed;
  }
  if (!socket_.valid()) {
    error = "stream has no socket";
    return StreamOutcome::Closed;
  }
  if (buffer_.size() < kFrameHeaderBytes + max_payload_bytes_) {
    buffer_.resize(kFrameHeaderBytes + max_payload_bytes_);
  }
  const int wait_slice = wait_ms <= 0 ? 1 : (wait_ms > 50 ? 50 : wait_ms);
  int remaining = wait_ms;
  for (;;) {
    // Try to decode from what is already buffered.
    if (buffered_ >= kFrameHeaderBytes) {
      const std::uint32_t payload_length = get_u32(buffer_.data() + 12);
      if (payload_length > max_payload_bytes_) {
        error = "peer announced an oversized frame";
        closed_ = true;
        return StreamOutcome::ProtocolError;
      }
      const std::size_t total = kFrameHeaderBytes + static_cast<std::size_t>(payload_length);
      if (buffered_ >= total) {
        auto decoded = decode_frame(buffer_.data(), total, max_payload_bytes_);
        if (!decoded.ok()) {
          error = decoded.error().message();
          closed_ = true;
          return StreamOutcome::ProtocolError;
        }
        std::memmove(buffer_.data(), buffer_.data() + total, buffered_ - total);
        buffered_ -= total;
        frame = *decoded;
        return StreamOutcome::Frame;
      }
    }
    if (wait_ms >= 0 && remaining <= 0) {
      error = "no complete frame within the wait budget";
      return StreamOutcome::TimedOut;
    }
    std::string wait_error;
    const auto readable = socket_.wait_readable(wait_slice, wait_error);
    if (readable == Socket::AcceptOutcome::TimedOut) {
      if (wait_ms >= 0) {
        remaining -= wait_slice;
      }
      continue;
    }
    if (readable != Socket::AcceptOutcome::Accepted) {
      error = wait_error.empty() ? std::string("socket closed while waiting for a frame") : wait_error;
      closed_ = true;
      return StreamOutcome::Closed;
    }
    const std::size_t space = buffer_.size() - buffered_;
    if (space == 0) {
      error = "frame buffer exhausted";
      closed_ = true;
      return StreamOutcome::ProtocolError;
    }
    std::size_t received = 0;
    Status read = socket_.receive_some(buffer_.data() + buffered_, space, received);
    if (!read.ok()) {
      error = read.error().message();
      closed_ = true;
      return StreamOutcome::Failed;
    }
    if (received == 0) {
      error = "peer closed the connection";
      closed_ = true;
      return StreamOutcome::Closed;
    }
    buffered_ += received;
  }
}

Status FrameStream::close() {
  closed_ = true;
  return socket_.close();
}

}  // namespace drain
