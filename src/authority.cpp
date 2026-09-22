#include "drain/authority.hpp"

#include <algorithm>

namespace drain {
namespace {

std::uint64_t read_u64(const JsonValue& value, std::string_view key, std::uint64_t fallback = 0) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_uint(fallback);
}

std::int64_t read_i64(const JsonValue& value, std::string_view key, std::int64_t fallback = 0) {
  const JsonValue* found = value.find(key);
  if (found == nullptr || !found->is_number()) {
    return fallback;
  }
  return found->as_int(fallback);
}

}  // namespace

AuthorityToken::AuthorityToken(AuthorityId id, DomainId scope, Generation generation, Epoch epoch,
                               IncarnationId incarnation, TimePoint issued_at, TimePoint expires_at)
    : id_(id),
      scope_(std::move(scope)),
      generation_(generation),
      epoch_(epoch),
      incarnation_(incarnation),
      issued_at_(issued_at),
      expires_at_(expires_at) {}

std::string AuthorityToken::label() const {
  std::string out = "authority:";
  out.append(drain::to_string(id_));
  out.append(" scope=");
  out.append(scope_.empty() ? std::string("*") : scope_.str());
  out.append(" generation=");
  out.append(drain::to_string(generation_));
  out.append(" epoch=");
  out.append(drain::to_string(epoch_));
  out.append(" incarnation=");
  out.append(drain::to_string(incarnation_));
  return out;
}

JsonValue AuthorityToken::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("id", JsonValue(id_.value()));
  value.set("scope", JsonValue(scope_.str()));
  value.set("generation", JsonValue(generation_.value()));
  value.set("epoch", JsonValue(epoch_.value()));
  value.set("incarnation", JsonValue(incarnation_.value()));
  value.set("issued_at_ns", JsonValue(static_cast<std::uint64_t>(issued_at_.count())));
  value.set("expires_at_ns", JsonValue(static_cast<std::uint64_t>(expires_at_.count())));
  value.set("revoked", JsonValue(revoked_));
  value.set("revoked_at_ns", JsonValue(static_cast<std::uint64_t>(revoked_at_.count())));
  value.set("revocation_reason", JsonValue(revocation_reason_));
  return value;
}

Result<AuthorityToken> AuthorityToken::from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::PersistenceCorrupt, "authority token is not an object");
  }
  AuthorityToken token;
  token.id_ = AuthorityId{read_u64(value, "id")};
  if (token.id_.is_zero()) {
    return Error(ErrorCode::PersistenceCorrupt, "authority token has a reserved zero id");
  }
  const std::string scope = value.get_string("scope");
  if (!scope.empty()) {
    const auto parsed = DomainId::parse(scope);
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "authority token has a malformed scope");
    }
    token.scope_ = *parsed;
  }
  token.generation_ = Generation{read_u64(value, "generation")};
  token.epoch_ = Epoch{read_u64(value, "epoch")};
  token.incarnation_ = IncarnationId{read_u64(value, "incarnation")};
  token.issued_at_ = TimePoint{read_i64(value, "issued_at_ns")};
  token.expires_at_ = TimePoint{read_i64(value, "expires_at_ns")};
  token.revoked_ = value.get_bool("revoked", false);
  token.revoked_at_ = TimePoint{read_i64(value, "revoked_at_ns")};
  token.revocation_reason_ = value.get_string("revocation_reason");
  // Recovered tokens are always revoked: authority never survives a restart.
  token.revoked_ = true;
  if (token.revocation_reason_.empty()) {
    token.revocation_reason_ = "recovered from snapshot; authority does not survive restart";
  }
  return token;
}

Result<AuthorityToken> AuthorityToken::from_presentation_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::InvalidArgument,
                 "a presented authority must be a token object, not a bare identifier");
  }
  AuthorityToken token;
  token.id_ = AuthorityId{read_u64(value, "id")};
  if (token.id_.is_zero()) {
    return Error(ErrorCode::AuthorityRequired, "presented authority token has no identifier");
  }
  const std::string scope = value.get_string("scope");
  if (!scope.empty()) {
    const auto parsed = DomainId::parse(scope);
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "presented authority token has a malformed scope");
    }
    token.scope_ = *parsed;
  }
  token.generation_ = Generation{read_u64(value, "generation")};
  token.epoch_ = Epoch{read_u64(value, "epoch")};
  token.incarnation_ = IncarnationId{read_u64(value, "incarnation")};
  token.issued_at_ = TimePoint{read_i64(value, "issued_at_ns")};
  token.expires_at_ = TimePoint{read_i64(value, "expires_at_ns")};
  return token;
}

const char* to_string(AuthorityRejection rejection) {
  switch (rejection) {
    case AuthorityRejection::None: return "none";
    case AuthorityRejection::Unknown: return "unknown";
    case AuthorityRejection::Revoked: return "revoked";
    case AuthorityRejection::Expired: return "expired";
    case AuthorityRejection::StaleEpoch: return "stale-epoch";
    case AuthorityRejection::StaleIncarnation: return "stale-incarnation";
    case AuthorityRejection::StaleGeneration: return "stale-generation";
  }
  return "unknown";
}

Status AuthorityRegistry::install_incarnation(IncarnationId incarnation, TimePoint at) {
  (void)at;
  if (incarnation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "incarnation id must be non-zero");
  }
  if (incarnation <= incarnation_) {
    return fail(ErrorCode::StaleIncarnation,
                "incarnation id must advance: it is a fresh-incarnation fence, not a label");
  }
  incarnation_ = incarnation;
  return ok_status();
}

Status AuthorityRegistry::begin_new_epoch(IncarnationId incarnation, TimePoint at, std::string reason) {
  if (incarnation <= incarnation_) {
    return fail(ErrorCode::StaleIncarnation, "incarnation id must advance across restarts");
  }
  const auto next_epoch = increment_checked(epoch_.value());
  if (!next_epoch.has_value()) {
    return fail(ErrorCode::BoundsExceeded, "authority epoch space exhausted");
  }
  epoch_ = Epoch{*next_epoch};
  incarnation_ = incarnation;
  Status revoked = revoke_all(at, std::move(reason));
  if (!revoked.ok()) {
    return revoked;
  }
  return ok_status();
}

Status AuthorityRegistry::install_line(IncarnationId incarnation, Epoch epoch, TimePoint at,
                                      std::string reason) {
  if (incarnation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "incarnation id must be non-zero");
  }
  if (epoch.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "authority epoch must be non-zero");
  }
  if (!incarnation_.is_zero() && incarnation <= incarnation_) {
    return fail(ErrorCode::StaleIncarnation, "the authority line must advance its incarnation");
  }
  if (epoch < epoch_) {
    return fail(ErrorCode::StaleEpoch, "the authority line must not rewind its epoch");
  }
  incarnation_ = incarnation;
  epoch_ = epoch;
  return revoke_all(at, std::move(reason));
}

Result<AuthorityToken> AuthorityRegistry::issue(const DomainId& scope, TimePoint at, Duration validity) {
  if (incarnation_.is_zero()) {
    return Error(ErrorCode::AuthorityRequired, "no incarnation is installed; authority cannot be issued");
  }
  if (tokens_.size() >= kMaxAuthorityTokens) {
    return Error(ErrorCode::BoundsExceeded, "authority token limit reached");
  }
  const auto next = increment_checked(high_water_.value());
  if (!next.has_value()) {
    return Error(ErrorCode::BoundsExceeded, "authority id space exhausted");
  }
  high_water_ = AuthorityId{*next};
  const TimePoint expires_at =
      validity.count() == 0 ? TimePoint{0} : saturating_add(at, validity);
  AuthorityToken token(high_water_, scope, scope_generation(scope), epoch_, incarnation_, at, expires_at);
  tokens_.emplace(high_water_, token);
  return token;
}

Result<Generation> AuthorityRegistry::advance_scope_generation(const DomainId& scope) {
  const auto it = scope_generations_.find(scope);
  const std::uint64_t current = it == scope_generations_.end() ? 0 : it->second.value();
  const auto next = increment_checked(current);
  if (!next.has_value()) {
    return Error(ErrorCode::BoundsExceeded, "scope generation space exhausted");
  }
  scope_generations_[scope] = Generation{*next};
  return Generation{*next};
}

Generation AuthorityRegistry::scope_generation(const DomainId& scope) const {
  const auto it = scope_generations_.find(scope);
  return it == scope_generations_.end() ? Generation{} : it->second;
}

AuthorityRejection AuthorityRegistry::check(const AuthorityToken& token, TimePoint now) const {
  const auto it = tokens_.find(token.id());
  if (it == tokens_.end()) {
    return AuthorityRejection::Unknown;
  }
  if (it->second.revoked()) {
    return AuthorityRejection::Revoked;
  }
  if (token.incarnation() != incarnation_) {
    return AuthorityRejection::StaleIncarnation;
  }
  if (token.epoch() != epoch_) {
    return AuthorityRejection::StaleEpoch;
  }
  if (token.generation() != scope_generation(token.scope())) {
    return AuthorityRejection::StaleGeneration;
  }
  if (token.expired_at(now)) {
    return AuthorityRejection::Expired;
  }
  return AuthorityRejection::None;
}

Status AuthorityRegistry::validate(const AuthorityToken& token, TimePoint now) const {
  switch (check(token, now)) {
    case AuthorityRejection::None:
      return ok_status();
    case AuthorityRejection::Unknown:
      return fail(ErrorCode::AuthorityRequired, "authority token is not known to this incarnation");
    case AuthorityRejection::Revoked:
      return fail(ErrorCode::StaleAuthority, "authority token was revoked");
    case AuthorityRejection::Expired:
      return fail(ErrorCode::StaleAuthority, "authority token expired");
    case AuthorityRejection::StaleEpoch:
      return fail(ErrorCode::StaleEpoch, "authority token belongs to a superseded epoch");
    case AuthorityRejection::StaleIncarnation:
      return fail(ErrorCode::StaleIncarnation,
                  "authority token was issued by a previous incarnation and cannot be reused");
    case AuthorityRejection::StaleGeneration:
      return fail(ErrorCode::StaleGeneration, "authority token belongs to a superseded scope generation");
  }
  return fail(ErrorCode::Internal, "unreachable authority check result");
}

Status AuthorityRegistry::revoke(AuthorityId id, TimePoint at, std::string reason) {
  const auto it = tokens_.find(id);
  if (it == tokens_.end()) {
    return fail(ErrorCode::NotFound, "authority token is not known");
  }
  it->second.revoke(at, std::move(reason));
  return ok_status();
}

Status AuthorityRegistry::revoke_all(TimePoint at, std::string reason) {
  for (auto& [id, token] : tokens_) {
    (void)id;
    if (!token.revoked()) {
      token.revoke(at, reason);
    }
  }
  return ok_status();
}

const AuthorityToken* AuthorityRegistry::find(AuthorityId id) const {
  const auto it = tokens_.find(id);
  return it == tokens_.end() ? nullptr : &it->second;
}

std::vector<AuthorityToken> AuthorityRegistry::tokens() const {
  std::vector<AuthorityToken> out;
  out.reserve(tokens_.size());
  for (const auto& [id, token] : tokens_) {
    (void)id;
    out.push_back(token);
  }
  return out;
}

JsonValue AuthorityRegistry::to_json() const {
  JsonValue value = JsonValue::object();
  value.set("high_water", JsonValue(high_water_.value()));
  value.set("incarnation", JsonValue(incarnation_.value()));
  value.set("epoch", JsonValue(epoch_.value()));
  JsonValue scopes = JsonValue::array();
  for (const auto& [scope, generation] : scope_generations_) {
    JsonValue entry = JsonValue::object();
    entry.set("scope", JsonValue(scope.str()));
    entry.set("generation", JsonValue(generation.value()));
    scopes.array_ref().push_back(std::move(entry));
  }
  value.set("scope_generations", std::move(scopes));
  JsonValue tokens = JsonValue::array();
  for (const auto& [id, token] : tokens_) {
    (void)id;
    tokens.array_ref().push_back(token.to_json());
  }
  value.set("tokens", std::move(tokens));
  return value;
}

Status AuthorityRegistry::load_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return fail(ErrorCode::PersistenceCorrupt, "authority payload is not an object");
  }
  AuthorityRegistry staged;
  staged.high_water_ = AuthorityId{read_u64(value, "high_water")};
  staged.incarnation_ = IncarnationId{read_u64(value, "incarnation")};
  staged.epoch_ = Epoch{read_u64(value, "epoch", 1)};
  if (staged.epoch_.is_zero()) {
    return fail(ErrorCode::PersistenceCorrupt, "authority epoch must be non-zero");
  }
  const JsonValue* scopes = value.find("scope_generations");
  if (scopes != nullptr && scopes->is_array()) {
    if (scopes->size() > kMaxAuthorityTokens) {
      return fail(ErrorCode::BoundsExceeded, "authority scope count exceeds bound");
    }
    for (const auto& entry : scopes->as_array()) {
      const std::string scope = entry.get_string("scope");
      Generation generation{read_u64(entry, "generation")};
      DomainId domain;
      if (!scope.empty()) {
        const auto parsed = DomainId::parse(scope);
        if (!parsed.has_value()) {
          return fail(ErrorCode::MalformedIdentity, "authority scope entry is malformed");
        }
        domain = *parsed;
      }
      staged.scope_generations_[domain] = generation;
    }
  }
  const JsonValue* tokens = value.find("tokens");
  if (tokens != nullptr && tokens->is_array()) {
    if (tokens->size() > kMaxAuthorityTokens) {
      return fail(ErrorCode::BoundsExceeded, "authority token count exceeds bound");
    }
    for (const auto& entry : tokens->as_array()) {
      auto token = AuthorityToken::from_json(entry);
      if (!token.ok()) {
        return fail(ErrorCode::PersistenceCorrupt, "authority token rejected: " + token.error().message());
      }
      if (staged.high_water_ < token->id()) {
        staged.high_water_ = token->id();
      }
      staged.tokens_.emplace(token->id(), *token);
    }
  }
  *this = std::move(staged);
  return ok_status();
}

void AuthorityRegistry::clear() {
  tokens_.clear();
  scope_generations_.clear();
  high_water_ = AuthorityId{};
  incarnation_ = IncarnationId{};
  epoch_ = Epoch{1};
}

}  // namespace drain
