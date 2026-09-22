#pragma once

// Drain Fabric -- wire protocol.
//
// Messages are framed, versioned, length-delimited, and integrity checked:
//
//   offset 0   magic        u32  0x44524652 ("DRFR")
//   offset 4   version      u16  wire protocol version
//   offset 6   type         u16  message type
//   offset 8   sequence     u32  request/response correlation
//   offset 12  payload_len  u32  bytes of UTF-8 JSON
//   offset 16  payload_crc  u32  CRC-32C over the payload
//   offset 20  header_crc   u32  CRC-32C over bytes [0, 20)
//   offset 24  payload      payload_len bytes
//
// A frame that is short, mis-framed, oversized, or whose checksums do not match
// is rejected without being applied. The stream keeps partial reads so a
// bounded wait never discards bytes.

#include <cstdint>
#include <string>
#include <vector>

#include "drain/export.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"
#include "drain/socket.hpp"
#include "drain/version.hpp"

namespace drain {

inline constexpr std::uint32_t kFrameMagic = 0x44524652u;
inline constexpr std::size_t kFrameHeaderBytes = 24;
inline constexpr std::size_t kMaxFramePayloadBytes = 4u * 1024u * 1024u;

enum class MessageType : std::uint16_t {
  Hello = 1,
  Welcome = 2,
  Failure = 3,
  DrainRequest = 10,
  DrainResponse = 11,
  StatusRequest = 12,
  StatusResponse = 13,
  CancelRequest = 14,
  CancelResponse = 15,
  RestoreRequest = 16,
  RestoreResponse = 17,
  ExplainRequest = 18,
  ExplainResponse = 19,
  ObligationQuery = 20,
  ObligationQueryResponse = 21,
  ObligationAdmit = 26,
  ObligationAdmitResponse = 27,
  ObligationReport = 22,
  ObligationReportResponse = 23,
  EvidenceReport = 24,
  EvidenceResponse = 25,
  EvacuationRequest = 30,
  EvacuationResponse = 31,
  AdmissionReopen = 32,
  AdmissionReopenResponse = 33,
  AuditRequest = 34,
  AuditResponse = 35,
  Shutdown = 40,
  ShutdownResponse = 41,
};

DRAIN_API const char* to_string(MessageType type);
DRAIN_API std::optional<MessageType> message_type_from_string(std::string_view text);

struct Frame {
  MessageType type{MessageType::Hello};
  std::uint32_t sequence{0};
  JsonValue payload{};

  JsonValue to_json() const;
};

/// Encodes a frame. Rejects payloads larger than max_payload_bytes.
DRAIN_API Result<std::vector<unsigned char>> encode_frame(const Frame& frame,
                                                          std::size_t max_payload_bytes);

/// Decodes a complete frame image.
DRAIN_API Result<Frame> decode_frame(const unsigned char* data, std::size_t length,
                                     std::size_t max_payload_bytes);

enum class StreamOutcome : std::uint8_t {
  Frame = 0,
  TimedOut = 1,
  Closed = 2,
  Failed = 3,
  ProtocolError = 4,
};

DRAIN_API const char* to_string(StreamOutcome outcome);

/// Length-exact framed stream over a socket. Partial reads are retained across
/// calls, so a timed-out read never loses bytes.
class FrameStream {
 public:
  FrameStream() = default;
  explicit FrameStream(Socket socket, std::size_t max_payload_bytes = kMaxFramePayloadBytes);

  FrameStream(FrameStream&&) noexcept = default;
  FrameStream& operator=(FrameStream&&) noexcept = default;
  FrameStream(const FrameStream&) = delete;
  FrameStream& operator=(const FrameStream&) = delete;

  Status send_frame(const Frame& frame);

  StreamOutcome receive_frame(Frame& frame, int wait_ms, std::string& error);

  Status close();
  bool valid() const noexcept { return socket_.valid(); }
  Socket& socket() noexcept { return socket_; }
  const Socket& socket() const noexcept { return socket_; }
  std::size_t max_payload_bytes() const noexcept { return max_payload_bytes_; }

 private:
  Socket socket_{};
  std::size_t max_payload_bytes_{kMaxFramePayloadBytes};
  std::vector<unsigned char> buffer_{};
  std::size_t buffered_{0};
  bool closed_{false};
};

}  // namespace drain
