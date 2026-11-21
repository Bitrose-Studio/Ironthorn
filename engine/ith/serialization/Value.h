#pragma once

// serialization::Value: a small tree of JSON values for free-form data (ADR-0010).
//
// Migrations edit documents as Value trees before reflection reads them, and code that handles
// data without a reflected type (editor commands, config) can use it directly:
//
//   ith::serialization::Value config = ith::serialization::Value::object();
//   config.set("name", ith::serialization::Value::string("Ironthorn"));
//   config.set("width", ith::serialization::Value::integer(1920));
//   if (const ith::serialization::Value* width = config.find("width")) {
//       const std::optional<std::int32_t> pixels = width->toNumber<std::int32_t>();
//   }
//
// Numbers keep the text of their JSON token, so 64-bit integers and floats keep every digit
// until they are converted to a C++ type. Objects keep their members in order, duplicates
// included; find() returns the first one.
//
// Trees must stay within kMaxDepth (Document.h) levels of arrays and objects: the writer refuses
// deeper trees, and copying, comparing and destroying a tree recurse once per level.
//
// Values own their children. A Value is not thread-safe; distinct Values may be used from
// different threads.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace ith::serialization {

// True when `text` is a number as RFC 8259 writes it: an optional '-', an integer part without
// leading zeros, an optional fraction and an optional exponent.
[[nodiscard]] bool isJsonNumber(std::string_view text) noexcept;

class Value {
public:
    enum class Kind : std::uint8_t {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    struct Member;

    // A null value.
    Value() noexcept = default;

    [[nodiscard]] static Value boolean(bool value);
    // A number from the text of a JSON number token. Precondition (asserted): isJsonNumber(text);
    // otherwise the result is the number 0.
    [[nodiscard]] static Value number(std::string_view text);
    [[nodiscard]] static Value integer(std::int64_t value);
    [[nodiscard]] static Value unsignedInteger(std::uint64_t value);
    // The shortest text that reads back as the same double. Precondition (asserted): the value is
    // finite, since JSON has no infinities or NaN; otherwise the result is the number 0.
    [[nodiscard]] static Value real(double value);
    // A string. The text should be UTF-8; writing a value with invalid UTF-8 fails.
    [[nodiscard]] static Value string(std::string text);
    [[nodiscard]] static Value array();
    [[nodiscard]] static Value array(std::vector<Value> elements);
    [[nodiscard]] static Value object();

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }

    // The value of a Bool. Precondition (asserted): kind() == Kind::Bool; otherwise false.
    [[nodiscard]] bool asBool() const noexcept;

    // The token of a Number, e.g. "-12.5e3". Precondition (asserted): kind() == Kind::Number;
    // otherwise empty.
    [[nodiscard]] std::string_view numberText() const noexcept;

    // The number converted to T (an integer or floating-point type). nullopt when this is not a
    // Number, when an integer type is asked for a number with a fraction or exponent, or when
    // the value is outside T's range.
    template <typename T>
    [[nodiscard]] std::optional<T> toNumber() const noexcept;

    // The text of a String. Precondition (asserted): kind() == Kind::String; otherwise empty.
    [[nodiscard]] const std::string& asString() const noexcept;

    // The elements of an Array. Precondition (asserted): kind() == Kind::Array.
    [[nodiscard]] std::vector<Value>& elements() noexcept;
    [[nodiscard]] const std::vector<Value>& elements() const noexcept;

    // The members of an Object in document order. Precondition (asserted): kind() == Kind::Object.
    [[nodiscard]] std::vector<Member>& members() noexcept;
    [[nodiscard]] const std::vector<Member>& members() const noexcept;

    // The first member of an Object with this key; null when there is none or this is not an
    // Object. The pointer is valid until the members change.
    [[nodiscard]] Value* find(std::string_view key) noexcept;
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;

    // Replaces the first member with this key, or appends one. Precondition (asserted): this is
    // an Object. Returns the stored value.
    Value& set(std::string key, Value value);

    // Removes every member with this key; true when there was one. Precondition (asserted): this
    // is an Object.
    bool remove(std::string_view key);

    // Deep equality. Numbers compare by their text ("1.0" != "1"), object members in order.
    friend bool operator==(const Value& a, const Value& b);

private:
    Kind m_kind = Kind::Null;
    bool m_bool = false;
    std::string m_text; // Number and String
    std::vector<Value> m_elements;
    std::vector<Member> m_members;
};

// One member of an Object.
struct Value::Member {
    std::string key;
    Value value;

    friend bool operator==(const Member&, const Member&) = default;
};

template <typename T>
std::optional<T> Value::toNumber() const noexcept {
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, "toNumber<T>: T must be a number type");
    if (m_kind != Kind::Number) {
        return std::nullopt;
    }
    T result{};
    const char* const end = m_text.data() + m_text.size();
    const std::from_chars_result parsed = std::from_chars(m_text.data(), end, result);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        return std::nullopt;
    }
    return result;
}

} // namespace ith::serialization
