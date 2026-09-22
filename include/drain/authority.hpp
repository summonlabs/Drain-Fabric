#pragma once

// Drain Fabric -- authority and incarnation fencing.
//
// Every mutating operation is performed under an authority token. A token is
// only valid while its scope generation, authority epoch, and producing
// incarnation all match the live ones. Restarting the runtime bumps the epoch
// and installs a new incarnation, which invalidates every token issued before
// the restart: deserialized authority is never resurrected.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "drain/clock.hpp"
#include "drain/export.hpp"
#include "drain/identity.hpp"
#include "drain/json.hpp"
#include "drain/result.hpp"

namespace drain {

inline constexpr std::size_t kMaxAuthorityTokens = 65536;

/// Where an authority applies. A scope is a failure domain or, for cluster-wide
/// operations, the empty domain. Generation is per scope and is advanced
/// whenever the authority line for that scope is re-established.
class AuthorityToken {
 public:
  AuthorityToken() = default;
  AuthorityToken(AuthorityId id, DomainId scope, Generation generation, Epoch epoch,
                 IncarnationId incarnation, TimePoint issued_at, TimePoint expires_at);

  AuthorityId id() const noexcept { return id_; }
  const DomainId& scope() const noexcept { return scope_; }
  Generation generation() const noexcept { return generation_; }
  Epoch epoch() const noexcept { return epoch_; }
  IncarnationId incarnation() const noexcept { return incarnation_; }
  TimePoint issued_at() const noexcept { return issued_at_; }
  TimePoint expires_at() const noexcept { return expires_at_; }
  bool revoked() const noexcept { return revoked_; }
  TimePoint revoked_at() const noexcept { return revoked_at_; }
  const std::string& revocation_reason() const noexcept { return revocation_reason_; }

  bool expired_at(TimePoint now) const noexcept {
    return expires_at_.count() != 0 && now > expires_at_;
  }

  void revoke(TimePoint at, std::string reason) {
    revoked_ = true;
    revoked_at_ = at;
    revocation_reason_ = std::move(reason);
  }

  std::string label() const;
  JsonValue to_json() const;
  static Result<AuthorityToken> from_json(const JsonValue& value);

  /// Parses a token that a peer *presents*. Unlike snapshot recovery, a
  /// presentation is not force-revoked: it is fenced by comparing its
  /// incarnation, epoch, generation, and expiry against the live line.
  static Result<AuthorityToken> from_presentation_json(const JsonValue& value);

 private:
  AuthorityId id_{};
  DomainId scope_{};
  Generation generation_{};
  Epoch epoch_{};
  IncarnationId incarnation_{};
  TimePoint issued_at_{0};
  TimePoint expires_at_{0};
  bool revoked_{false};
  TimePoint revoked_at_{0};
  std::string revocation_reason_{};
};

/// Why a token was rejected. Reported so that the caller can distinguish "you
/// are stale, re-read state" from "you were never allowed to do this".
enum class AuthorityRejection : std::uint8_t {
  None = 0,
  Unknown = 1,
  Revoked = 2,
  Expired = 3,
  StaleEpoch = 4,
  StaleIncarnation = 5,
  StaleGeneration = 6,
};

DRAIN_API const char* to_string(AuthorityRejection rejection);

class AuthorityRegistry {
 public:
  AuthorityRegistry() = default;

  IncarnationId incarnation() const noexcept { return incarnation_; }
  Epoch epoch() const noexcept { return epoch_; }

  /// Installs the boot incarnation. Rejected when it does not advance the
  /// previous incarnation, which would allow a restarted process to reuse the
  /// authority line of its predecessor.
  Status install_incarnation(IncarnationId incarnation, TimePoint at);

  /// Advances the epoch and installs a new incarnation, revoking every token.
  /// This is the restart fence.
  Status begin_new_epoch(IncarnationId incarnation, TimePoint at, std::string reason);

  /// Installs an explicit authority line. Both the incarnation and the epoch
  /// must advance relative to whatever the registry currently holds (or the
  /// registry must be empty). Every existing token is revoked. Used by the
  /// runtime so that a hard kill cannot rewind the epoch.
  Status install_line(IncarnationId incarnation, Epoch epoch, TimePoint at, std::string reason);

  Result<AuthorityToken> issue(const DomainId& scope, TimePoint at, Duration validity);

  /// Bumps the generation for a scope, invalidating every token previously
  /// issued for it. Used when admission for a target is reopened.
  Result<Generation> advance_scope_generation(const DomainId& scope);

  Generation scope_generation(const DomainId& scope) const;

  AuthorityRejection check(const AuthorityToken& token, TimePoint now) const;

  /// Convenience wrapper producing a Result-friendly status.
  Status validate(const AuthorityToken& token, TimePoint now) const;

  Status revoke(AuthorityId id, TimePoint at, std::string reason);
  Status revoke_all(TimePoint at, std::string reason);

  const AuthorityToken* find(AuthorityId id) const;

  std::vector<AuthorityToken> tokens() const;
  std::size_t size() const noexcept { return tokens_.size(); }

  JsonValue to_json() const;
  Status load_from_json(const JsonValue& value);

  void clear();

 private:
  std::map<AuthorityId, AuthorityToken> tokens_{};
  std::map<DomainId, Generation> scope_generations_{};
  AuthorityId high_water_{};
  IncarnationId incarnation_{};
  Epoch epoch_{Epoch{1}};
};

}  // namespace drain
