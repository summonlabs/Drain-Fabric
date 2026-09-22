#pragma once

// Drain Fabric -- deterministic JSON.
//
// Used for snapshot payloads, wire messages, CLI output, and explanation
// rendering. The writer preserves member insertion order so that byte-identical
// inputs always produce byte-identical output (a property the property tests
// rely on). The parser is strict, bounded, and total: it never throws and never
// allocates without a limit.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "drain/checked.hpp"
#include "drain/export.hpp"
#include "drain/result.hpp"

namespace drain {

/// Hard bounds applied to every parse. Sizes come from untrusted input, so they
/// are checked before allocation.
struct JsonLimits {
  std::size_t max_depth = 32;
  std::size_t max_string_bytes = 8192;
  std::size_t max_container_entries = 65536;
  std::size_t max_nodes = 262144;
  std::size_t max_total_bytes = 8u * 1024u * 1024u;
};

class JsonValue {
 public:
  using Array = std::vector<JsonValue>;

  /// Ordered object. Keys are stored in insertion order; lookup is linear
  /// because object sizes are bounded by JsonLimits.
  class Object {
   public:
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    const std::string& key(std::size_t index) const;
    const JsonValue& value(std::size_t index) const;
    JsonValue& value(std::size_t index);
    void push_back(std::string key, JsonValue value);
    void clear() noexcept;

   private:
    std::vector<std::string> keys_;
    std::vector<JsonValue> values_;
  };

  enum class Type : std::uint8_t { Null = 0, Bool = 1, Int = 2, Uint = 3, Real = 4, String = 5, Array = 6, Object = 7 };

  JsonValue() = default;
  JsonValue(std::nullptr_t) {}
  JsonValue(bool value) : storage_(value) {}
  JsonValue(int value) : storage_(static_cast<std::int64_t>(value)) {}
  JsonValue(std::int64_t value) : storage_(value) {}
  JsonValue(std::uint64_t value) : storage_(value) {}
  JsonValue(double value) : storage_(value) {}
  JsonValue(std::string value) : storage_(std::move(value)) {}
  JsonValue(const char* value) : storage_(std::string(value)) {}
  JsonValue(Array value) : storage_(std::move(value)) {}
  JsonValue(Object value) : storage_(std::move(value)) {}

  static JsonValue array() { return JsonValue(Array{}); }
  static JsonValue object() { return JsonValue(Object{}); }

  Type type() const noexcept { return static_cast<Type>(storage_.index()); }
  bool is_null() const noexcept { return type() == Type::Null; }
  bool is_bool() const noexcept { return type() == Type::Bool; }
  bool is_int() const noexcept { return type() == Type::Int; }
  bool is_uint() const noexcept { return type() == Type::Uint; }
  bool is_number() const noexcept { return is_int() || is_uint() || is_real(); }
  bool is_real() const noexcept { return type() == Type::Real; }
  bool is_string() const noexcept { return type() == Type::String; }
  bool is_array() const noexcept { return type() == Type::Array; }
  bool is_object() const noexcept { return type() == Type::Object; }

  bool as_bool(bool fallback = false) const noexcept;
  std::int64_t as_int(std::int64_t fallback = 0) const noexcept;
  std::uint64_t as_uint(std::uint64_t fallback = 0) const noexcept;
  double as_real(double fallback = 0.0) const noexcept;
  const std::string& as_string() const noexcept;
  Array& as_array() noexcept;
  const Array& as_array() const noexcept;
  Object& as_object() noexcept;
  const Object& as_object() const noexcept;

  Array& array_ref() noexcept { return std::get<Array>(storage_); }
  Object& object_ref() noexcept { return std::get<Object>(storage_); }
  const Array& array_ref() const noexcept { return std::get<Array>(storage_); }
  const Object& object_ref() const noexcept { return std::get<Object>(storage_); }

  /// Appends or replaces a member. A non-object value is promoted to an object
  /// so that callers cannot corrupt the shape silently.
  void set(std::string key, JsonValue value);
  /// Looks up a member; returns nullptr when absent or when this is not an object.
  const JsonValue* find(std::string_view key) const noexcept;
  JsonValue* find(std::string_view key) noexcept;
  std::string get_string(std::string_view key, std::string fallback = {}) const;
  std::uint64_t get_uint(std::string_view key, std::uint64_t fallback = 0) const noexcept;
  std::int64_t get_int(std::string_view key, std::int64_t fallback = 0) const noexcept;
  bool get_bool(std::string_view key, bool fallback = false) const noexcept;

  std::size_t size() const noexcept;

  /// Deterministic serialization. indent = 0 produces a compact single line.
  std::string dump(int indent = 0) const;

 private:
  std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object> storage_{nullptr};
};

inline std::size_t JsonValue::Object::size() const noexcept { return keys_.size(); }
inline bool JsonValue::Object::empty() const noexcept { return keys_.empty(); }
inline const std::string& JsonValue::Object::key(std::size_t index) const { return keys_[index]; }
inline const JsonValue& JsonValue::Object::value(std::size_t index) const { return values_[index]; }
inline JsonValue& JsonValue::Object::value(std::size_t index) { return values_[index]; }
inline void JsonValue::Object::push_back(std::string key, JsonValue value) {
  keys_.push_back(std::move(key));
  values_.push_back(std::move(value));
}
inline void JsonValue::Object::clear() noexcept {
  keys_.clear();
  values_.clear();
}

/// Strict parser. Rejects trailing content, duplicate object keys, control
/// characters in strings, leading zeros, and any input exceeding the limits.
DRAIN_API Result<JsonValue> json_parse(std::string_view text, const JsonLimits& limits = JsonLimits{});

/// Escapes a string as a JSON string literal (including surrounding quotes).
DRAIN_API std::string json_escape(std::string_view text);

}  // namespace drain
