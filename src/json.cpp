#include "drain/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace drain {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_escaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (c < 0x20u) {
          out.append("\\u00");
          out.push_back(kHexDigits[(c >> 4) & 0xfu]);
          out.push_back(kHexDigits[c & 0xfu]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void append_number(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out.append("0");
    return;
  }
  std::array<char, 64> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
  if (written <= 0) {
    out.append("0");
    return;
  }
  out.append(buffer.data(), static_cast<std::size_t>(written));
}

void dump_into(std::string& out, const JsonValue& value, int indent, std::size_t depth) {
  const std::string pad(indent > 0 ? static_cast<std::size_t>(indent) * depth : 0, ' ');
  const std::string pad_inner(indent > 0 ? static_cast<std::size_t>(indent) * (depth + 1) : 0, ' ');
  const char* newline = indent > 0 ? "\n" : "";
  switch (value.type()) {
    case JsonValue::Type::Null: out.append("null"); break;
    case JsonValue::Type::Bool: out.append(value.as_bool() ? "true" : "false"); break;
    case JsonValue::Type::Int: out.append(std::to_string(value.as_int())); break;
    case JsonValue::Type::Uint: out.append(std::to_string(value.as_uint())); break;
    case JsonValue::Type::Real: append_number(out, value.as_real()); break;
    case JsonValue::Type::String: append_escaped(out, value.as_string()); break;
    case JsonValue::Type::Array: {
      const auto& items = value.array_ref();
      if (items.empty()) {
        out.append("[]");
        break;
      }
      out.push_back('[');
      bool first = true;
      for (const auto& item : items) {
        if (!first) out.push_back(',');
        first = false;
        out.append(newline);
        out.append(pad_inner);
        dump_into(out, item, indent, depth + 1);
      }
      out.append(newline);
      out.append(pad);
      out.push_back(']');
      break;
    }
    case JsonValue::Type::Object: {
      const auto& members = value.object_ref();
      if (members.empty()) {
        out.append("{}");
        break;
      }
      out.push_back('{');
      bool first = true;
      for (std::size_t index = 0; index < members.size(); ++index) {
        if (!first) out.push_back(',');
        first = false;
        out.append(newline);
        out.append(pad_inner);
        append_escaped(out, members.key(index));
        out.append(indent > 0 ? ": " : ":");
        dump_into(out, members.value(index), indent, depth + 1);
      }
      out.append(newline);
      out.append(pad);
      out.push_back('}');
      break;
    }
  }
}

class Parser {
 public:
  Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

  Result<JsonValue> run() {
    if (text_.size() > limits_.max_total_bytes) {
      return Error(ErrorCode::BoundsExceeded, "json input exceeds max_total_bytes");
    }
    skip_whitespace();
    JsonValue value;
    auto parsed = parse_value(0, value);
    if (!parsed.ok()) {
      return parsed.error();
    }
    skip_whitespace();
    if (pos_ != text_.size()) {
      return Error(ErrorCode::ProtocolError, "trailing content after json value");
    }
    return value;
  }

 private:
  static Status fail(ErrorCode code, std::string message) { return Status(Error(code, std::move(message))); }

  void skip_whitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  Status count_node() {
    ++nodes_;
    if (nodes_ > limits_.max_nodes) {
      return fail(ErrorCode::BoundsExceeded, "json node budget exceeded");
    }
    return ok_status();
  }

  Status parse_value(std::size_t depth, JsonValue& out) {
    if (depth > limits_.max_depth) {
      return fail(ErrorCode::BoundsExceeded, "json nesting depth exceeded");
    }
    auto counted = count_node();
    if (!counted.ok()) {
      return counted;
    }
    if (pos_ >= text_.size()) {
      return fail(ErrorCode::ProtocolError, "unexpected end of json input");
    }
    const char c = text_[pos_];
    switch (c) {
      case '{': return parse_object(depth, out);
      case '[': return parse_array(depth, out);
      case '"': {
        std::string text;
        auto parsed = parse_string(text);
        if (!parsed.ok()) return parsed;
        out = JsonValue(std::move(text));
        return ok_status();
      }
      case 't': return parse_literal("true", JsonValue(true), out);
      case 'f': return parse_literal("false", JsonValue(false), out);
      case 'n': return parse_literal("null", JsonValue(nullptr), out);
      default: return parse_number(out);
    }
  }

  Status parse_literal(std::string_view literal, JsonValue value, JsonValue& out) {
    if (text_.compare(pos_, literal.size(), literal) != 0) {
      return fail(ErrorCode::ProtocolError, "invalid json literal");
    }
    pos_ += literal.size();
    out = std::move(value);
    return ok_status();
  }

  Status parse_object(std::size_t depth, JsonValue& out) {
    ++pos_;
    JsonValue::Object members;
    skip_whitespace();
    if (consume('}')) {
      out = JsonValue(std::move(members));
      return ok_status();
    }
    for (;;) {
      skip_whitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return fail(ErrorCode::ProtocolError, "expected object key");
      }
      std::string key;
      auto parsed_key = parse_string(key);
      if (!parsed_key.ok()) return parsed_key;
      if (members.size() >= limits_.max_container_entries) {
        return fail(ErrorCode::BoundsExceeded, "json object entry budget exceeded");
      }
      for (std::size_t index = 0; index < members.size(); ++index) {
        if (members.key(index) == key) {
          return fail(ErrorCode::ProtocolError, "duplicate json object key");
        }
      }
      skip_whitespace();
      if (!consume(':')) {
        return fail(ErrorCode::ProtocolError, "expected ':' in json object");
      }
      skip_whitespace();
      JsonValue value;
      auto parsed_value = parse_value(depth + 1, value);
      if (!parsed_value.ok()) return parsed_value;
      members.push_back(std::move(key), std::move(value));
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        break;
      }
      return fail(ErrorCode::ProtocolError, "expected ',' or '}' in json object");
    }
    out = JsonValue(std::move(members));
    return ok_status();
  }

  Status parse_array(std::size_t depth, JsonValue& out) {
    ++pos_;
    JsonValue::Array items;
    skip_whitespace();
    if (consume(']')) {
      out = JsonValue(std::move(items));
      return ok_status();
    }
    for (;;) {
      if (items.size() >= limits_.max_container_entries) {
        return fail(ErrorCode::BoundsExceeded, "json array entry budget exceeded");
      }
      skip_whitespace();
      JsonValue value;
      auto parsed = parse_value(depth + 1, value);
      if (!parsed.ok()) return parsed;
      items.push_back(std::move(value));
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        break;
      }
      return fail(ErrorCode::ProtocolError, "expected ',' or ']' in json array");
    }
    out = JsonValue(std::move(items));
    return ok_status();
  }

  Status parse_hex4(std::uint32_t& out) {
    if (pos_ + 4u > text_.size()) {
      return fail(ErrorCode::ProtocolError, "truncated unicode escape");
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const char c = text_[pos_ + i];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a') + 10u;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A') + 10u;
      } else {
        return fail(ErrorCode::ProtocolError, "invalid hex digit in unicode escape");
      }
      value = (value << 4) | digit;
    }
    pos_ += 4u;
    out = value;
    return ok_status();
  }

  static void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80u) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800u) {
      out.push_back(static_cast<char>(0xc0u | (code >> 6)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    } else if (code < 0x10000u) {
      out.push_back(static_cast<char>(0xe0u | (code >> 12)));
      out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    } else {
      out.push_back(static_cast<char>(0xf0u | (code >> 18)));
      out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    }
  }

  Status parse_string(std::string& out) {
    ++pos_;
    for (;;) {
      if (pos_ >= text_.size()) {
        return fail(ErrorCode::ProtocolError, "unterminated json string");
      }
      const auto c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return ok_status();
      }
      if (c < 0x20u) {
        return fail(ErrorCode::ProtocolError, "unescaped control character in json string");
      }
      if (c != '\\') {
        if (out.size() >= limits_.max_string_bytes) {
          return fail(ErrorCode::BoundsExceeded, "json string length budget exceeded");
        }
        out.push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      ++pos_;
      if (pos_ >= text_.size()) {
        return fail(ErrorCode::ProtocolError, "truncated json escape");
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t code = 0;
          auto parsed = parse_hex4(code);
          if (!parsed.ok()) return parsed;
          if (code >= 0xd800u && code <= 0xdbffu) {
            if (pos_ + 2u > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1u] != 'u') {
              return fail(ErrorCode::ProtocolError, "lone high surrogate in json string");
            }
            pos_ += 2u;
            std::uint32_t low = 0;
            auto parsed_low = parse_hex4(low);
            if (!parsed_low.ok()) return parsed_low;
            if (low < 0xdc00u || low > 0xdfffu) {
              return fail(ErrorCode::ProtocolError, "invalid low surrogate in json string");
            }
            code = 0x10000u + ((code - 0xd800u) << 10) + (low - 0xdc00u);
          } else if (code >= 0xdc00u && code <= 0xdfffu) {
            return fail(ErrorCode::ProtocolError, "unpaired low surrogate in json string");
          }
          append_utf8(out, code);
          break;
        }
        default:
          return fail(ErrorCode::ProtocolError, "invalid json escape");
      }
      if (out.size() > limits_.max_string_bytes) {
        return fail(ErrorCode::BoundsExceeded, "json string length budget exceeded");
      }
    }
  }

  Status parse_number(JsonValue& out) {
    const std::size_t start = pos_;
    if (consume('-')) {
      // sign consumed
    }
    if (pos_ >= text_.size()) {
      return fail(ErrorCode::ProtocolError, "truncated json number");
    }
    if (text_[pos_] == '0') {
      ++pos_;
      if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        return fail(ErrorCode::ProtocolError, "leading zero in json number");
      }
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      return fail(ErrorCode::ProtocolError, "invalid json number");
    }
    bool integral = true;
    if (consume('.')) {
      integral = false;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return fail(ErrorCode::ProtocolError, "missing fraction digits in json number");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      integral = false;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return fail(ErrorCode::ProtocolError, "missing exponent digits in json number");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    const std::string_view token = text_.substr(start, pos_ - start);
    if (integral && token.size() <= 20u) {
      if (!token.empty() && token.front() == '-') {
        std::int64_t signed_value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), signed_value);
        if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) {
          out = JsonValue(signed_value);
          return ok_status();
        }
      } else {
        std::uint64_t unsigned_value = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), unsigned_value);
        if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) {
          out = JsonValue(unsigned_value);
          return ok_status();
        }
      }
    }
    const std::string owned(token);
    char* end = nullptr;
    const double real = std::strtod(owned.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(real)) {
      return fail(ErrorCode::ProtocolError, "json number out of range");
    }
    out = JsonValue(real);
    return ok_status();
  }

  std::string_view text_;
  JsonLimits limits_;
  std::size_t pos_{0};
  std::size_t nodes_{0};
};

}  // namespace

bool JsonValue::as_bool(bool fallback) const noexcept {
  if (const auto* value = std::get_if<bool>(&storage_)) return *value;
  return fallback;
}

std::int64_t JsonValue::as_int(std::int64_t fallback) const noexcept {
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) return *value;
  if (const auto* value = std::get_if<std::uint64_t>(&storage_)) {
    return *value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
               ? static_cast<std::int64_t>(*value)
               : fallback;
  }
  return fallback;
}

std::uint64_t JsonValue::as_uint(std::uint64_t fallback) const noexcept {
  if (const auto* value = std::get_if<std::uint64_t>(&storage_)) return *value;
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) {
    return *value >= 0 ? static_cast<std::uint64_t>(*value) : fallback;
  }
  return fallback;
}

double JsonValue::as_real(double fallback) const noexcept {
  if (const auto* value = std::get_if<double>(&storage_)) return *value;
  if (const auto* value = std::get_if<std::int64_t>(&storage_)) return static_cast<double>(*value);
  if (const auto* value = std::get_if<std::uint64_t>(&storage_)) return static_cast<double>(*value);
  return fallback;
}

const std::string& JsonValue::as_string() const noexcept {
  static const std::string kEmpty;
  if (const auto* value = std::get_if<std::string>(&storage_)) return *value;
  return kEmpty;
}

JsonValue::Array& JsonValue::as_array() noexcept {
  static Array kEmpty;
  kEmpty.clear();
  if (auto* value = std::get_if<Array>(&storage_)) return *value;
  return kEmpty;
}

const JsonValue::Array& JsonValue::as_array() const noexcept {
  static const Array kEmpty;
  if (const auto* value = std::get_if<Array>(&storage_)) return *value;
  return kEmpty;
}

JsonValue::Object& JsonValue::as_object() noexcept {
  if (auto* value = std::get_if<Object>(&storage_)) return *value;
  storage_ = Object{};
  return std::get<Object>(storage_);
}

const JsonValue::Object& JsonValue::as_object() const noexcept {
  static const Object kEmpty;
  if (const auto* value = std::get_if<Object>(&storage_)) return *value;
  return kEmpty;
}

void JsonValue::set(std::string key, JsonValue value) {
  if (!is_object()) {
    storage_ = Object{};
  }
  auto& members = std::get<Object>(storage_);
  for (std::size_t index = 0; index < members.size(); ++index) {
    if (members.key(index) == key) {
      members.value(index) = std::move(value);
      return;
    }
  }
  members.push_back(std::move(key), std::move(value));
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (!is_object()) {
    return nullptr;
  }
  const auto& members = std::get<Object>(storage_);
  for (std::size_t index = 0; index < members.size(); ++index) {
    if (members.key(index) == key) {
      return &members.value(index);
    }
  }
  return nullptr;
}

JsonValue* JsonValue::find(std::string_view key) noexcept {
  if (!is_object()) {
    return nullptr;
  }
  auto& members = std::get<Object>(storage_);
  for (std::size_t index = 0; index < members.size(); ++index) {
    if (members.key(index) == key) {
      return &members.value(index);
    }
  }
  return nullptr;
}

std::string JsonValue::get_string(std::string_view key, std::string fallback) const {
  const JsonValue* found = find(key);
  if (found != nullptr && found->is_string()) {
    return found->as_string();
  }
  return fallback;
}

std::uint64_t JsonValue::get_uint(std::string_view key, std::uint64_t fallback) const noexcept {
  const JsonValue* found = find(key);
  return found != nullptr ? found->as_uint(fallback) : fallback;
}

std::int64_t JsonValue::get_int(std::string_view key, std::int64_t fallback) const noexcept {
  const JsonValue* found = find(key);
  return found != nullptr ? found->as_int(fallback) : fallback;
}

bool JsonValue::get_bool(std::string_view key, bool fallback) const noexcept {
  const JsonValue* found = find(key);
  return found != nullptr ? found->as_bool(fallback) : fallback;
}

std::size_t JsonValue::size() const noexcept {
  switch (type()) {
    case Type::Array: return std::get<Array>(storage_).size();
    case Type::Object: return std::get<Object>(storage_).size();
    case Type::String: return std::get<std::string>(storage_).size();
    default: return 0;
  }
}

std::string JsonValue::dump(int indent) const {
  std::string out;
  dump_into(out, *this, indent, 0);
  return out;
}

std::string json_escape(std::string_view text) {
  std::string out;
  append_escaped(out, text);
  return out;
}

Result<JsonValue> json_parse(std::string_view text, const JsonLimits& limits) {
  Parser parser(text, limits);
  return parser.run();
}

}  // namespace drain
