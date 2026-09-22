#include "drain/agent.hpp"

#include <utility>

namespace drain {

AgentSession::AgentSession(AgentOptions options, Clock& clock)
    : options_(std::move(options)), clock_(clock) {}

Status AgentSession::connect() {
  auto client = WireClient::connect_to(options_.host, options_.port, SessionRole::Agent, options_.node,
                                       options_.max_frame_bytes, options_.wait_ms);
  if (!client.ok()) {
    return client.error();
  }
  client_ = std::move(*client);
  return ok_status();
}

Status AgentSession::register_obligations() {
  if (!client_.valid()) {
    return fail(ErrorCode::TransportError, "the agent session is not connected");
  }
  for (const auto& obligation : options_.obligations) {
    JsonValue payload = JsonValue::object();
    payload.set("obligation", obligation.to_json());
    payload.set("generation", JsonValue(Generation{}.value()));
    // The whole session token is presented, not just its identifier, so the
    // controller fences it against the live authority line.
    payload.set("authority", client_.session_token().to_json());
    auto response = client_.call(MessageType::ObligationAdmit, std::move(payload), options_.wait_ms);
    if (!response.ok()) {
      return response.error();
    }
    registered_ids_.push_back(ObligationId{response->get_uint("obligation", 0)});
    ++admissions_registered_;
  }
  if (!registered_ids_.empty()) {
    JsonValue reopen = JsonValue::object();
    reopen.set("node", JsonValue(options_.node));
    auto response = client_.call(MessageType::AdmissionReopen, std::move(reopen), options_.wait_ms);
    (void)response;
  }
  return ok_status();
}

Status AgentSession::handle_evacuation(const Frame& frame) {
  ++evacuations_handled_;
  const std::uint64_t drain_id = frame.payload.get_uint("drain", 0);
  const std::uint64_t obligation_id = frame.payload.get_uint("obligation", 0);
  const std::uint64_t generation = frame.payload.get_uint("generation", 0);
  const std::uint64_t expected_revision = frame.payload.get_uint("expected_revision", 0);
  const std::uint64_t reserved_evidence = frame.payload.get_uint("evidence_seq", 0);

  if (options_.fail_evacuations) {
    JsonValue report = JsonValue::object();
    report.set("obligation", JsonValue(obligation_id));
    report.set("expected_revision", JsonValue(expected_revision));
    report.set("next", JsonValue(std::string(drain::to_string(ObligationState::Failed))));
    report.set("evidence", JsonValue(static_cast<std::uint64_t>(0)));
    report.set("drain", JsonValue(drain_id));
    report.set("generation", JsonValue(generation));
    report.set("note", JsonValue("the adjacent runtime reported evacuation failure"));
    JsonValue envelope = JsonValue::object();
    envelope.set("report", std::move(report));
    auto response = client_.call(MessageType::ObligationReport, std::move(envelope), options_.wait_ms);
    if (!response.ok()) {
      return response.error();
    }
    JsonValue acknowledge = JsonValue::object();
    acknowledge.set("ok", JsonValue(true));
    acknowledge.set("released", JsonValue(false));
    return client_.send(MessageType::EvacuationResponse, std::move(acknowledge));
  }
  if (options_.ignore_evacuations) {
    JsonValue acknowledge = JsonValue::object();
    acknowledge.set("ok", JsonValue(true));
    acknowledge.set("released", JsonValue(false));
    return client_.send(MessageType::EvacuationResponse, std::move(acknowledge));
  }

  EvidenceSeq released_seq{};
  if (reserved_evidence != 0) {
    drain::Evidence evidence;
    evidence.seq = EvidenceSeq{reserved_evidence};
    evidence.key.kind = EvidenceKind::ObligationReleased;
    evidence.key.drain = DrainId{drain_id};
    evidence.key.obligation = ObligationId{obligation_id};
    evidence.generation = Generation{generation};
    evidence.epoch = Epoch{client_.server_epoch()};
    evidence.producer = IncarnationId{client_.server_incarnation()};
    evidence.observed_at = clock_.now();
    evidence.healthy = true;
    evidence.detail = "agent released the obligation on request from the controller";
    JsonValue envelope = JsonValue::object();
    envelope.set("evidence", evidence.to_json());
    auto response = client_.call(MessageType::EvidenceReport, std::move(envelope), options_.wait_ms);
    if (!response.ok()) {
      return response.error();
    }
    released_seq = EvidenceSeq{response->get_uint("seq", 0)};
  }

  JsonValue report = JsonValue::object();
  report.set("obligation", JsonValue(obligation_id));
  report.set("expected_revision", JsonValue(expected_revision));
  report.set("next", JsonValue(std::string(drain::to_string(ObligationState::Released))));
  report.set("evidence", JsonValue(released_seq.value()));
  report.set("drain", JsonValue(drain_id));
  report.set("generation", JsonValue(generation));
  report.set("note", JsonValue("agent completed the requested release"));
  JsonValue envelope = JsonValue::object();
  envelope.set("report", std::move(report));
  auto response = client_.call(MessageType::ObligationReport, std::move(envelope), options_.wait_ms);
  if (!response.ok()) {
    return response.error();
  }

  JsonValue acknowledge = JsonValue::object();
  acknowledge.set("ok", JsonValue(true));
  acknowledge.set("released", JsonValue(true));
  return client_.send(MessageType::EvacuationResponse, std::move(acknowledge));
}

Status AgentSession::handle_admission_reopen(const Frame& frame) {
  ++reopens_handled_;
  JsonValue acknowledge = JsonValue::object();
  acknowledge.set("ok", JsonValue(true));
  acknowledge.set("generation", frame.payload.find("generation") == nullptr
                                    ? JsonValue(static_cast<std::uint64_t>(0))
                                    : *frame.payload.find("generation"));
  return client_.send(MessageType::AdmissionReopenResponse, std::move(acknowledge));
}

Status AgentSession::poll_once() {
  if (!client_.valid()) {
    return fail(ErrorCode::TransportError, "the agent session is not connected");
  }
  Frame frame;
  std::string error;
  const auto outcome = client_.receive(frame, options_.wait_ms, error);
  switch (outcome) {
    case StreamOutcome::TimedOut:
      return ok_status();
    case StreamOutcome::Frame:
      break;
    case StreamOutcome::Closed:
      return fail(ErrorCode::TransportError, "the controller closed the agent session");
    case StreamOutcome::Failed:
    case StreamOutcome::ProtocolError:
      return fail(ErrorCode::ProtocolError, "agent session protocol error: " + error);
  }
  switch (frame.type) {
    case MessageType::EvacuationRequest:
      return handle_evacuation(frame);
    case MessageType::AdmissionReopen:
      return handle_admission_reopen(frame);
    case MessageType::Failure:
      return fail(ErrorCode::ProtocolError, "controller refused: " + frame.payload.get_string("message"));
    default:
      return ok_status();
  }
}

Status AgentSession::run_until(std::size_t handled_budget) {
  while (!stopping_) {
    if (handled_budget != 0 && evacuations_handled_ >= handled_budget) {
      break;
    }
    Status step = poll_once();
    if (!step.ok()) {
      return step;
    }
  }
  return ok_status();
}

Status AgentSession::stop() {
  stopping_ = true;
  return client_.close();
}

}  // namespace drain
