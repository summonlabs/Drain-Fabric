#include "drain/identity.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace drain {
namespace {

constexpr char kKindSeparator = ':';
constexpr char kDomainSeparator = '@';

bool is_slug_char(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= 'a' && c <= 'z') return true;
  if (c >= '0' && c <= '9') return true;
  return c == '_' || c == '.' || c == ':' || c == '-';
}

}  // namespace

bool is_valid_slug(std::string_view text) {
  if (text.empty() || text.size() > kMaxSlugLength) {
    return false;
  }
  for (const char raw : text) {
    if (!is_slug_char(static_cast<unsigned char>(raw))) {
      return false;
    }
  }
  return true;
}

const char* to_string(TargetKind kind) {
  switch (kind) {
    case TargetKind::Port: return "port";
    case TargetKind::Link: return "link";
    case TargetKind::Switch: return "switch";
    case TargetKind::Path: return "path";
    case TargetKind::Service: return "service";
    case TargetKind::Resource: return "resource";
  }
  return "unknown";
}

std::optional<TargetKind> target_kind_from_string(std::string_view text) {
  if (text == "port") return TargetKind::Port;
  if (text == "link") return TargetKind::Link;
  if (text == "switch") return TargetKind::Switch;
  if (text == "path") return TargetKind::Path;
  if (text == "service") return TargetKind::Service;
  if (text == "resource") return TargetKind::Resource;
  return std::nullopt;
}

DrainTarget::DrainTarget(TargetKind kind, ResourceId id, DomainId domain)
    : kind_(kind), id_(std::move(id)), domain_(std::move(domain)) {}

std::string DrainTarget::to_string() const {
  std::string out;
  out.reserve(24);
  out.append(drain::to_string(kind_));
  out.push_back(kKindSeparator);
  out.append(id_.str());
  out.push_back(kDomainSeparator);
  out.append(domain_.str());
  return out;
}

std::optional<DrainTarget> DrainTarget::parse(std::string_view text) {
  const std::size_t sep = text.find(kKindSeparator);
  if (sep == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view kind_text = text.substr(0, sep);
  std::string_view rest = text.substr(sep + 1);
  std::string_view id_text = rest;
  std::string_view domain_text{};
  const std::size_t at = rest.find(kDomainSeparator);
  if (at != std::string_view::npos) {
    id_text = rest.substr(0, at);
    domain_text = rest.substr(at + 1);
  }
  const auto kind = target_kind_from_string(kind_text);
  if (!kind.has_value()) {
    return std::nullopt;
  }
  auto id = ResourceId::parse(id_text);
  if (!id.has_value()) {
    return std::nullopt;
  }
  DomainId domain;
  if (!domain_text.empty()) {
    auto parsed_domain = DomainId::parse(domain_text);
    if (!parsed_domain.has_value()) {
      return std::nullopt;
    }
    domain = *parsed_domain;
  }
  return DrainTarget(*kind, *id, domain);
}

bool target_order_less(const DrainTarget& a, const DrainTarget& b) {
  if (a.domain() != b.domain()) {
    return a.domain() < b.domain();
  }
  const auto ak = static_cast<std::uint8_t>(a.kind());
  const auto bk = static_cast<std::uint8_t>(b.kind());
  if (ak != bk) {
    return ak < bk;
  }
  return a.id() < b.id();
}

JsonValue to_json(const DrainTarget& target) {
  JsonValue value = JsonValue::object();
  value.set("kind", JsonValue(std::string(drain::to_string(target.kind()))));
  value.set("id", JsonValue(target.id().str()));
  value.set("domain", JsonValue(target.domain().str()));
  return value;
}

Result<DrainTarget> drain_target_from_json(const JsonValue& value) {
  if (!value.is_object()) {
    return Error(ErrorCode::ProtocolError, "drain target must be a json object");
  }
  const JsonValue* kind_value = value.find("kind");
  const JsonValue* id_value = value.find("id");
  if (kind_value == nullptr || !kind_value->is_string() || id_value == nullptr || !id_value->is_string()) {
    return Error(ErrorCode::MalformedIdentity, "drain target requires string kind and id");
  }
  const auto kind = target_kind_from_string(kind_value->as_string());
  if (!kind.has_value()) {
    return Error(ErrorCode::MalformedIdentity, "unknown drain target kind");
  }
  const auto id = ResourceId::parse(id_value->as_string());
  if (!id.has_value()) {
    return Error(ErrorCode::MalformedIdentity, "malformed drain target resource id");
  }
  DomainId domain;
  const JsonValue* domain_value = value.find("domain");
  if (domain_value != nullptr && domain_value->is_string() && !domain_value->as_string().empty()) {
    const auto parsed = DomainId::parse(domain_value->as_string());
    if (!parsed.has_value()) {
      return Error(ErrorCode::MalformedIdentity, "malformed drain target failure domain");
    }
    domain = *parsed;
  }
  return DrainTarget(*kind, *id, domain);
}

std::string to_string(const ResourceId& id) { return id.str(); }
std::string to_string(const DomainId& id) { return id.str(); }
std::string to_string(const HolderId& id) { return id.str(); }
std::string to_string(const GroupId& id) { return id.str(); }
std::string to_string(const ServiceId& id) { return id.str(); }
std::string to_string(const NodeId& id) { return id.str(); }

}  // namespace drain
