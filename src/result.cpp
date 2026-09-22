#include "drain/result.hpp"

namespace drain {

const char* to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    // NOLINTNEXTLINE(bugprone-branch-clone): the enum-to-string table is explicit on purpose.
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::MalformedIdentity: return "malformed-identity";
    case ErrorCode::NotFound: return "not-found";
    case ErrorCode::AlreadyExists: return "already-exists";
    case ErrorCode::DuplicateIdentity: return "duplicate-identity";
    case ErrorCode::StaleGeneration: return "stale-generation";
    case ErrorCode::StaleAuthority: return "stale-authority";
    case ErrorCode::StaleEpoch: return "stale-epoch";
    case ErrorCode::StaleIncarnation: return "stale-incarnation";
    case ErrorCode::StaleEvidence: return "stale-evidence";
    case ErrorCode::StaleRevision: return "stale-revision";
    case ErrorCode::AdmissionClosed: return "admission-closed";
    case ErrorCode::IllegalTransition: return "illegal-transition";
    case ErrorCode::PolicyViolation: return "policy-violation";
    case ErrorCode::RedundancyViolation: return "redundancy-violation";
    case ErrorCode::CapacityViolation: return "capacity-violation";
    case ErrorCode::DomainLimitViolation: return "domain-limit-violation";
    case ErrorCode::ProtectedObligationRemains: return "protected-obligation-remains";
    case ErrorCode::AuthorityRequired: return "authority-required";
    case ErrorCode::PersistenceCorrupt: return "persistence-corrupt";
    case ErrorCode::PersistenceIo: return "persistence-io";
    case ErrorCode::BoundsExceeded: return "bounds-exceeded";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::ProtocolError: return "protocol-error";
    case ErrorCode::TransportError: return "transport-error";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::ShuttingDown: return "shutting-down";
    case ErrorCode::Internal: return "internal";
    case ErrorCode::Blocked: return "blocked";
  }
  return "unknown";
}

bool is_stale_fence(ErrorCode code) {
  switch (code) {
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleAuthority:
    case ErrorCode::StaleEpoch:
    case ErrorCode::StaleIncarnation:
    case ErrorCode::StaleEvidence:
    case ErrorCode::StaleRevision:
      return true;
    default:
      return false;
  }
}

bool is_retryable(ErrorCode code) {
  switch (code) {
    case ErrorCode::Busy:
    case ErrorCode::TransportError:
    case ErrorCode::PersistenceIo:
      return true;
    default:
      return false;
  }
}

std::string Error::to_string() const {
  if (code_ == ErrorCode::Ok) {
    return std::string("ok");
  }
  std::string out = drain::to_string(code_);
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  return out;
}

}  // namespace drain
