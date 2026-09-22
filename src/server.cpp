#include "drain/server.hpp"

#include <algorithm>
#include <utility>

#include "drain/explain.hpp"

namespace drain {
namespace {

JsonValue ok_payload() {
  JsonValue value = JsonValue::object();
  value.set("ok", JsonValue(true));
  return value;
}

JsonValue failure_payload(ErrorCode code, const std::string& message) {
  JsonValue value = JsonValue::object();
  value.set("ok", JsonValue(false));
  value.set("code", JsonValue(std::string(drain::to_string(code))));
  value.set("message", JsonValue(message));
  return value;
}

Result<NodeId> parse_node(const JsonValue& payload, std::string_view key) {
  const std::string text = payload.get_string(key);
  if (text.empty()) {
    return NodeId{};
  }
  const auto parsed = NodeId::parse(text);
  if (!parsed.has_value()) {
    return Error(ErrorCode::MalformedIdentity, "malformed node identity");
  }
  return *parsed;
}

Result<std::vector<DrainTarget>> parse_targets(const JsonValue& payload) {
  const JsonValue* targets = payload.find("targets");
  if (targets == nullptr || !targets->is_array() || targets->size() == 0) {
    return Error(ErrorCode::InvalidArgument, "drain request must list at least one target");
  }
  if (targets->size() > kHardMaxTargetsPerRequest) {
    return Error(ErrorCode::BoundsExceeded, "drain request exceeds the maximum target count");
  }
  std::vector<DrainTarget> out;
  out.reserve(targets->size());
  for (const auto& entry : targets->as_array()) {
    auto target = drain_target_from_json(entry);
    if (!target.ok()) {
      return target.error();
    }
    out.push_back(*target);
  }
  return out;
}

JsonValue drain_summary(const DrainRecord& record) {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(record.id().value()));
  value.set("set", JsonValue(record.set().value()));
  value.set("target", drain::to_json(record.target()));
  value.set("state", JsonValue(std::string(drain::to_string(record.state()))));
  value.set("generation", JsonValue(record.generation().value()));
  value.set("revision", JsonValue(record.revision().value()));
  if (record.blocked_from().has_value()) {
    value.set("blocked_from", JsonValue(std::string(drain::to_string(*record.blocked_from()))));
  }
  JsonValue blockers = JsonValue::array();
  for (const auto& blocker : record.blockers()) {
    blockers.array_ref().push_back(blocker.to_json());
  }
  value.set("blockers", std::move(blockers));
  return value;
}

}  // namespace

const char* to_string(SessionRole role) {
  switch (role) {
    case SessionRole::Client: return "client";
    case SessionRole::Agent: return "agent";
  }
  return "unknown";
}

std::optional<SessionRole> session_role_from_string(std::string_view text) {
  if (text == "client") return SessionRole::Client;
  if (text == "agent") return SessionRole::Agent;
  return std::nullopt;
}

ControllerServer::ControllerServer(DrainEngine& engine, ServerOptions options, Clock& clock)
    : engine_(engine), options_(std::move(options)), clock_(clock) {}

ControllerServer::~ControllerServer() {
  (void)stop_workers();
  (void)stop();
}

Status ControllerServer::start() {
  if (listener_.valid()) {
    return ok_status();
  }
  std::uint16_t bound = 0;
  auto listener = Socket::listen_on(options_.bind_host, options_.port, bound,
                                    static_cast<int>(options_.max_connections));
  if (!listener.ok()) {
    return listener.error();
  }
  listener_ = std::move(*listener);
  port_ = bound;
  stopping_.store(false, std::memory_order_release);
  return ok_status();
}

Status ControllerServer::stop() {
  stopping_.store(true, std::memory_order_release);
  const auto connections = snapshot_connections();
  for (const auto& connection : connections) {
    std::lock_guard<std::mutex> lock(connection->mutex);
    connection->closing = true;
    (void)connection->stream.close();
  }
  (void)listener_.close();
  std::lock_guard<std::mutex> lock(registry_mutex_);
  connections_.clear();
  return ok_status();
}

void ControllerServer::register_connection(ConnectionPtr connection) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  connections_.push_back(std::move(connection));
}

Result<AuthorityId> ControllerServer::resolve_authority(const ConnectionPtr& connection,
                                                      const JsonValue& payload) {
  const JsonValue* presented = payload.find("authority");
  if (presented == nullptr) {
    if (connection->authority.is_zero()) {
      return Error(ErrorCode::AuthorityRequired, "this session has no authority");
    }
    return connection->authority;
  }
  auto token = AuthorityToken::from_presentation_json(*presented);
  if (!token.ok()) {
    return token.error();
  }
  Status valid = engine_.validate_authority(*token, clock_.now());
  if (!valid.ok()) {
    return valid.error();
  }
  return token->id();
}

std::vector<ControllerServer::ConnectionPtr> ControllerServer::snapshot_connections() const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  return connections_;
}

void ControllerServer::reap_connections() {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                    [](const ConnectionPtr& connection) {
                                      return connection->closing || !connection->stream.valid();
                                    }),
                     connections_.end());
}

std::size_t ControllerServer::connection_count() const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  return connections_.size();
}

std::size_t ControllerServer::agent_count() const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  std::size_t count = 0;
  for (const auto& connection : connections_) {
    if (connection->role == SessionRole::Agent) {
      ++count;
    }
  }
  return count;
}

Status ControllerServer::send_frame(const ConnectionPtr& connection, MessageType type,
                                    std::uint32_t sequence, JsonValue payload) {
  Frame frame;
  frame.type = type;
  frame.sequence = sequence;
  frame.payload = std::move(payload);
  return connection->stream.send_frame(frame);
}

Status ControllerServer::send_failure(const ConnectionPtr& connection, std::uint32_t sequence,
                                      ErrorCode code, const std::string& message) {
  return send_frame(connection, MessageType::Failure, sequence, failure_payload(code, message));
}

Status ControllerServer::push_to_agents(MessageType type, const JsonValue& payload) {
  const auto connections = snapshot_connections();
  std::size_t delivered = 0;
  for (const auto& connection : connections) {
    if (connection->role != SessionRole::Agent || !connection->greeted) {
      continue;
    }
    std::unique_lock<std::mutex> lock(connection->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      continue;
    }
    Frame frame;
    frame.type = type;
    frame.sequence = 0;
    frame.payload = payload;
    if (connection->stream.send_frame(frame).ok()) {
      ++delivered;
    }
  }
  if (delivered == 0) {
    return fail(ErrorCode::TransportError, "no agent session is available to receive the request");
  }
  return ok_status();
}

Status ControllerServer::on_evacuation_request(const EvacuationRequest& request) {
  JsonValue payload = JsonValue::object();
  payload.set("drain", JsonValue(request.drain.value()));
  payload.set("target", drain::to_json(request.target));
  payload.set("obligation", JsonValue(request.obligation.value()));
  payload.set("expected_revision", JsonValue(request.expected_revision.value()));
  // The controller reserves the evidence sequence so that the adjacent runtime
  // can answer with an observation the ledger will accept.
  payload.set("evidence_seq", JsonValue(request.evidence_seq.value()));
  payload.set("kind", JsonValue(std::string(drain::to_string(request.kind))));
  payload.set("mode", JsonValue(std::string(drain::to_string(request.mode))));
  payload.set("generation", JsonValue(request.generation.value()));
  payload.set("attempt", JsonValue(static_cast<std::uint64_t>(request.attempt.value())));
  payload.set("at_ns", JsonValue(static_cast<std::uint64_t>(request.at.count())));
  payload.set("reason", JsonValue(request.reason));
  payload.set("node", JsonValue(NodeId{}.str()));
  return push_to_agents(MessageType::EvacuationRequest, payload);
}

Status ControllerServer::on_restoration_request(const RestorationRequest& request) {
  (void)request;
  // Restoration is driven by the same admission-reopen frame; there is no
  // separate agent-side operation to request.
  return ok_status();
}

Status ControllerServer::on_admission_reopen(const AdmissionReopenRequest& request) {
  JsonValue payload = JsonValue::object();
  payload.set("target", drain::to_json(request.target));
  payload.set("generation", JsonValue(request.generation.value()));
  payload.set("at_ns", JsonValue(static_cast<std::uint64_t>(request.at.count())));
  payload.set("reason", JsonValue(request.reason));
  return push_to_agents(MessageType::AdmissionReopen, payload);
}

Status ControllerServer::poll_once() {
  if (!listener_.valid()) {
    return fail(ErrorCode::ShuttingDown, "controller server is not listening");
  }
  Socket accepted;
  std::string error;
  const auto outcome = listener_.accept_one(accepted, options_.accept_wait_ms, error);
  if (outcome == Socket::AcceptOutcome::Accepted) {
    if (connection_count() >= options_.max_connections) {
      FrameStream rejected_stream(std::move(accepted), options_.max_frame_bytes);
      Frame frame;
      frame.type = MessageType::Failure;
      frame.sequence = 0;
      frame.payload = failure_payload(ErrorCode::Busy, "connection limit reached");
      (void)rejected_stream.send_frame(frame);
      (void)rejected_stream.close();
      ++rejected_frames_;
    } else {
      auto connection = std::make_shared<Connection>();
      connection->stream = FrameStream(std::move(accepted), options_.max_frame_bytes);
      connection->peer = options_.bind_host;
      register_connection(connection);
    }
  } else if (outcome == Socket::AcceptOutcome::Failed) {
    return fail(ErrorCode::TransportError, error);
  }

  const auto connections = snapshot_connections();
  for (const auto& connection : connections) {
    (void)service_connection(connection);
  }
  (void)engine_.advance(clock_.now());
  reap_connections();
  return ok_status();
}

Status ControllerServer::run_until(std::size_t request_budget) {
  while (!stopping_.load(std::memory_order_acquire)) {
    if (request_budget != 0 && served_requests_.load(std::memory_order_relaxed) >= request_budget) {
      break;
    }
    Status step = poll_once();
    if (!step.ok()) {
      return step;
    }
  }
  return ok_status();
}

Status ControllerServer::start_workers(std::size_t count) {
  if (count > options_.max_worker_threads) {
    return fail(ErrorCode::BoundsExceeded, "worker count exceeds the configured limit");
  }
  if (!workers_.empty()) {
    return fail(ErrorCode::Busy, "workers are already running");
  }
  for (std::size_t index = 0; index < count; ++index) {
    workers_.emplace_back([this]() {
      while (!stopping_.load(std::memory_order_acquire)) {
        Status step = poll_once();
        if (!step.ok()) {
          return;
        }
      }
    });
  }
  return ok_status();
}

Status ControllerServer::stop_workers() {
  stopping_.store(true, std::memory_order_release);
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  return ok_status();
}

Status ControllerServer::service_connection(const ConnectionPtr& connection) {
  std::unique_lock<std::mutex> lock(connection->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return ok_status();
  }
  if (connection->closing || !connection->stream.valid()) {
    return ok_status();
  }
  Frame frame;
  std::string error;
  const auto outcome = connection->stream.receive_frame(frame, options_.read_wait_ms, error);
  switch (outcome) {
    case StreamOutcome::TimedOut:
      return ok_status();
    case StreamOutcome::Closed:
      connection->closing = true;
      (void)connection->stream.close();
      return ok_status();
    case StreamOutcome::Failed:
    case StreamOutcome::ProtocolError:
      ++rejected_frames_;
      connection->closing = true;
      (void)connection->stream.close();
      return ok_status();
    case StreamOutcome::Frame:
      break;
  }
  lock.unlock();
  Status handled = handle_frame(connection, frame);
  {
    std::unique_lock<std::mutex> relock(connection->mutex, std::try_to_lock);
    if (relock.owns_lock() && connection->closing) {
      (void)connection->stream.close();
    }
  }
  return handled;
}

Status ControllerServer::handle_frame(const ConnectionPtr& connection, const Frame& frame) {
  if (!connection->greeted && frame.type != MessageType::Hello) {
    ++rejected_frames_;
    (void)send_failure(connection, frame.sequence, ErrorCode::ProtocolError,
                       "the first frame on a session must be a hello");
    connection->closing = true;
    return ok_status();
  }
  if (frame.type != MessageType::Hello) {
    // The handshake is not counted as served work: the daemon request budget is
    // expressed in operator-visible requests.
    ++served_requests_;
  }

  switch (frame.type) {
    case MessageType::Hello: {
      const std::uint64_t version = frame.payload.get_uint("protocol_version", 0);
      if (version != kWireProtocolVersion) {
        (void)send_failure(connection, frame.sequence, ErrorCode::ProtocolError,
                           "wire protocol version mismatch");
        connection->closing = true;
        return ok_status();
      }
      const auto role = session_role_from_string(frame.payload.get_string("role", "client"));
      if (!role.has_value()) {
        (void)send_failure(connection, frame.sequence, ErrorCode::ProtocolError, "unknown session role");
        connection->closing = true;
        return ok_status();
      }
      auto node = parse_node(frame.payload, "node");
      if (!node.ok()) {
        (void)send_failure(connection, frame.sequence, ErrorCode::MalformedIdentity,
                           node.error().message());
        connection->closing = true;
        return ok_status();
      }
      const DomainId scope{};
      auto authority = engine_.issue_authority(scope, options_.authority_validity, clock_.now());
      if (!authority.ok()) {
        (void)send_failure(connection, frame.sequence, authority.error().code(),
                           authority.error().message());
        connection->closing = true;
        return ok_status();
      }
      connection->role = *role;
      connection->node = *node;
      connection->authority = authority->id();
      connection->greeted = true;
      JsonValue welcome = JsonValue::object();
      welcome.set("ok", JsonValue(true));
      welcome.set("protocol_version", JsonValue(static_cast<std::uint64_t>(kWireProtocolVersion)));
      welcome.set("incarnation", JsonValue(engine_.incarnation().value()));
      welcome.set("epoch", JsonValue(engine_.epoch().value()));
      welcome.set("authority", JsonValue(authority->id().value()));
      welcome.set("authority_token", authority->to_json());
      welcome.set("role", JsonValue(std::string(drain::to_string(*role))));
      return send_frame(connection, MessageType::Welcome, frame.sequence, std::move(welcome));
    }
    case MessageType::DrainRequest: {
      auto targets = parse_targets(frame.payload);
      if (!targets.ok()) {
        return send_failure(connection, frame.sequence, targets.error().code(), targets.error().message());
      }
      auto node = parse_node(frame.payload, "node");
      if (!node.ok()) {
        return send_failure(connection, frame.sequence, node.error().code(), node.error().message());
      }
      DrainRequest request;
      request.targets = *targets;
      request.reason = frame.payload.get_string("reason");
      auto presented = resolve_authority(connection, frame.payload);
      if (!presented.ok()) {
        return send_failure(connection, frame.sequence, presented.error().code(),
                            presented.error().message());
      }
      request.authority = *presented;
      request.node = *node;
      request.nonce = RequestNonce{frame.payload.get_uint("nonce")};
      auto set = engine_.request_drain_set(request, clock_.now());
      if (!set.ok()) {
        return send_failure(connection, frame.sequence, set.error().code(), set.error().message());
      }
      JsonValue response = JsonValue::object();
      response.set("ok", JsonValue(true));
      response.set("set", JsonValue(set->value()));
      JsonValue drains = JsonValue::array();
      const auto stored = engine_.drain_set(*set);
      if (stored.has_value()) {
        for (const DrainId member : stored->members) {
          drains.array_ref().push_back(JsonValue(member.value()));
        }
      }
      response.set("drains", std::move(drains));
      return send_frame(connection, MessageType::DrainResponse, frame.sequence, std::move(response));
    }
    case MessageType::StatusRequest: {
      JsonValue response = JsonValue::object();
      response.set("ok", JsonValue(true));
      JsonValue drains = JsonValue::array();
      const std::uint64_t wanted = frame.payload.get_uint("drain", 0);
      if (wanted != 0) {
        auto record = engine_.drain(DrainId{wanted});
        if (!record.has_value()) {
          return send_failure(connection, frame.sequence, ErrorCode::NotFound, "no such drain");
        }
        drains.array_ref().push_back(drain_summary(*record));
      } else {
        for (const auto& record : engine_.drains()) {
          drains.array_ref().push_back(drain_summary(record));
        }
      }
      response.set("drains", std::move(drains));
      response.set("accounting", engine_.audit(clock_.now()).to_json());
      response.set("incarnation", JsonValue(engine_.incarnation().value()));
      response.set("epoch", JsonValue(engine_.epoch().value()));
      return send_frame(connection, MessageType::StatusResponse, frame.sequence, std::move(response));
    }
    case MessageType::CancelRequest: {
      const std::uint64_t wanted = frame.payload.get_uint("drain", 0);
      auto presented = resolve_authority(connection, frame.payload);
      if (!presented.ok()) {
        return send_failure(connection, frame.sequence, presented.error().code(),
                            presented.error().message());
      }
      Status cancelled = engine_.cancel_drain(DrainId{wanted}, *presented,
                                              frame.payload.get_string("reason"), clock_.now());
      if (!cancelled.ok()) {
        return send_failure(connection, frame.sequence, cancelled.error().code(),
                            cancelled.error().message());
      }
      return send_frame(connection, MessageType::CancelResponse, frame.sequence, ok_payload());
    }
    case MessageType::RestoreRequest: {
      const std::uint64_t wanted = frame.payload.get_uint("drain", 0);
      auto presented = resolve_authority(connection, frame.payload);
      if (!presented.ok()) {
        return send_failure(connection, frame.sequence, presented.error().code(),
                            presented.error().message());
      }
      Status restored = engine_.restore_drain(DrainId{wanted}, *presented,
                                              frame.payload.get_string("reason"), clock_.now());
      if (!restored.ok()) {
        return send_failure(connection, frame.sequence, restored.error().code(),
                            restored.error().message());
      }
      return send_frame(connection, MessageType::RestoreResponse, frame.sequence, ok_payload());
    }
    case MessageType::ExplainRequest: {
      const std::uint64_t wanted = frame.payload.get_uint("drain", 0);
      const Explanation explanation = engine_.explain(DrainId{wanted});
      JsonValue response = JsonValue::object();
      response.set("ok", JsonValue(true));
      response.set("explanation", explanation.to_json());
      response.set("text", JsonValue(explanation.render()));
      return send_frame(connection, MessageType::ExplainResponse, frame.sequence, std::move(response));
    }
    case MessageType::ObligationQuery: {
      JsonValue response = JsonValue::object();
      response.set("ok", JsonValue(true));
      JsonValue obligations = JsonValue::array();
      const JsonValue* target_value = frame.payload.find("target");
      if (target_value != nullptr) {
        auto target = drain_target_from_json(*target_value);
        if (!target.ok()) {
          return send_failure(connection, frame.sequence, target.error().code(), target.error().message());
        }
        for (const auto& obligation : engine_.outstanding_obligations(*target)) {
          obligations.array_ref().push_back(obligation.to_json());
        }
      } else {
        for (const auto& obligation : engine_.obligations()) {
          obligations.array_ref().push_back(obligation.to_json());
        }
      }
      response.set("obligations", std::move(obligations));
      return send_frame(connection, MessageType::ObligationQueryResponse, frame.sequence,
                        std::move(response));
    }
    case MessageType::ObligationAdmit: {
      const JsonValue* body = frame.payload.find("obligation");
      if (body == nullptr) {
        return send_failure(connection, frame.sequence, ErrorCode::InvalidArgument,
                            "admission frame lacks an obligation body");
      }
      auto obligation = Obligation::from_json(*body);
      if (!obligation.ok()) {
        return send_failure(connection, frame.sequence, obligation.error().code(),
                            obligation.error().message());
      }
      const Generation generation{frame.payload.get_uint("generation", 0)};
      auto presented = resolve_authority(connection, frame.payload);
      if (!presented.ok()) {
        return send_failure(connection, frame.sequence, presented.error().code(),
                            presented.error().message());
      }
      auto stored = engine_.admit_obligation(*obligation, generation, *presented, clock_.now());
      if (!stored.ok()) {
        return send_failure(connection, frame.sequence, stored.error().code(), stored.error().message());
      }
      JsonValue response = JsonValue::object();
      response.set("ok", JsonValue(true));
      response.set("obligation", JsonValue(stored->id().value()));
      return send_frame(connection, MessageType::ObligationAdmitResponse, frame.sequence,
                        std::move(response));
    }
    case MessageType::ObligationReport: {
      const JsonValue* body = frame.payload.find("report");
      if (body == nullptr) {
        return send_failure(connection, frame.sequence, ErrorCode::InvalidArgument,
                            "obligation report frame lacks a report body");
      }
      const auto next = obligation_state_from_string(body->get_string("next"));
      if (!next.has_value()) {
        return send_failure(connection, frame.sequence, ErrorCode::InvalidArgument,
                            "obligation report carries an unknown state");
      }
      ObligationReport report;
      report.obligation = ObligationId{body->get_uint("obligation")};
      report.expected_revision = Revision{body->get_uint("expected_revision")};
      report.next = *next;
      report.evidence = EvidenceSeq{body->get_uint("evidence")};
      report.drain = DrainId{body->get_uint("drain")};
      report.generation = Generation{body->get_uint("generation")};
      report.note = body->get_string("note");
      Status applied = engine_.report_obligation(report, clock_.now());
      if (!applied.ok()) {
        return send_failure(connection, frame.sequence, applied.error().code(), applied.error().message());
      }
      return send_frame(connection, MessageType::ObligationReportResponse, frame.sequence, ok_payload());
    }
    case MessageType::EvidenceReport: {
      const JsonValue* body = frame.payload.find("evidence");
      if (body == nullptr) {
        return send_failure(connection, frame.sequence, ErrorCode::InvalidArgument,
                            "evidence frame lacks an evidence body");
      }
      auto evidence = Evidence::from_json(*body);
      if (!evidence.ok()) {
        return send_failure(connection, frame.sequence, evidence.error().code(),
                            evidence.error().message());
      }
      auto recorded = engine_.record_evidence(*evidence, clock_.now());
      if (!recorded.ok()) {
        return send_failure(connection, frame.sequence, recorded.error().code(),
                            recorded.error().message());
      }
      JsonValue response = JsonValue::object();
      response.set("ok", JsonValue(true));
      response.set("seq", JsonValue(recorded->seq.value()));
      return send_frame(connection, MessageType::EvidenceResponse, frame.sequence, std::move(response));
    }
    case MessageType::AuditRequest: {
      JsonValue response = JsonValue::object();
      response.set("ok", JsonValue(true));
      response.set("accounting", engine_.audit(clock_.now()).to_json());
      response.set("stats", engine_.stats().to_json());
      return send_frame(connection, MessageType::AuditResponse, frame.sequence, std::move(response));
    }
    case MessageType::Shutdown: {
      (void)send_frame(connection, MessageType::ShutdownResponse, frame.sequence, ok_payload());
      stopping_.store(true, std::memory_order_release);
      return ok_status();
    }
    default:
      ++rejected_frames_;
      return send_failure(connection, frame.sequence, ErrorCode::ProtocolError,
                          "message type is not accepted by a controller session");
  }
}

WireClient::WireClient(FrameStream stream, JsonValue welcome)
    : stream_(std::move(stream)), welcome_(std::move(welcome)) {
  authority_ = AuthorityId{welcome_.get_uint("authority", 0)};
  server_incarnation_ = welcome_.get_uint("incarnation", 0);
  server_epoch_ = welcome_.get_uint("epoch", 0);
}

Result<WireClient> WireClient::connect_to(const std::string& host, std::uint16_t port, SessionRole role,
                                           const std::string& node, std::size_t max_frame_bytes,
                                           int wait_ms) {
  auto socket = Socket::connect_to(host, port);
  if (!socket.ok()) {
    return socket.error();
  }
  FrameStream stream(std::move(*socket), max_frame_bytes);
  WireClient client(std::move(stream), JsonValue::object());
  client.sequence_ = 0;

  JsonValue hello = JsonValue::object();
  hello.set("protocol_version", JsonValue(static_cast<std::uint64_t>(kWireProtocolVersion)));
  hello.set("role", JsonValue(std::string(drain::to_string(role))));
  hello.set("node", JsonValue(node));
  Frame request;
  request.type = MessageType::Hello;
  request.sequence = ++client.sequence_;
  request.payload = std::move(hello);
  Status sent = client.stream_.send_frame(request);
  if (!sent.ok()) {
    return sent.error();
  }
  Frame response;
  std::string error;
  const auto outcome = client.stream_.receive_frame(response, wait_ms, error);
  if (outcome != StreamOutcome::Frame) {
    return Error(ErrorCode::TransportError,
                 "no welcome frame from the controller: " + std::string(drain::to_string(outcome)) + " " + error);
  }
  if (response.type == MessageType::Failure) {
    return Error(ErrorCode::ProtocolError, response.payload.get_string("message", "handshake refused"));
  }
  if (response.type != MessageType::Welcome) {
    return Error(ErrorCode::ProtocolError, "handshake did not produce a welcome frame");
  }
  client.welcome_ = response.payload;
  client.authority_ = AuthorityId{response.payload.get_uint("authority", 0)};
  if (const JsonValue* token = response.payload.find("authority_token"); token != nullptr) {
    auto parsed = AuthorityToken::from_presentation_json(*token);
    if (parsed.ok()) {
      client.session_token_ = *parsed;
    }
  }
  client.server_incarnation_ = response.payload.get_uint("incarnation", 0);
  client.server_epoch_ = response.payload.get_uint("epoch", 0);
  return client;
}

Status WireClient::send(MessageType type, JsonValue payload) {
  Frame frame;
  frame.type = type;
  frame.sequence = ++sequence_;
  frame.payload = std::move(payload);
  return stream_.send_frame(frame);
}

StreamOutcome WireClient::receive(Frame& frame, int wait_ms, std::string& error) {
  return stream_.receive_frame(frame, wait_ms, error);
}

Result<JsonValue> WireClient::call(MessageType type, JsonValue payload, int wait_ms) {
  Status sent = send(type, std::move(payload));
  if (!sent.ok()) {
    return sent.error();
  }
  const std::uint32_t wanted = sequence_;
  for (;;) {
    Frame frame;
    std::string error;
    const auto outcome = stream_.receive_frame(frame, wait_ms, error);
    if (outcome != StreamOutcome::Frame) {
      return Error(ErrorCode::TransportError,
                   std::string("no response frame: ") + drain::to_string(outcome) + " " + error);
    }
    if (frame.sequence != wanted) {
      continue;
    }
    if (frame.type == MessageType::Failure) {
      const std::string code = frame.payload.get_string("code");
      ErrorCode mapped = ErrorCode::Internal;
      for (std::uint16_t raw = 0; raw <= 30; ++raw) {
        const auto candidate = static_cast<ErrorCode>(raw);
        if (code == drain::to_string(candidate)) {
          mapped = candidate;
          break;
        }
      }
      return Error(mapped, frame.payload.get_string("message"));
    }
    return frame.payload;
  }
}

Status WireClient::close() { return stream_.close(); }

}  // namespace drain
