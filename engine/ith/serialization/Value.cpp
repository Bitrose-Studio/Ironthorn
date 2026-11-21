#include "ith/serialization/Value.h"

#include "ith/foundation/Assert.h"

#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <utility>

namespace ith::serialization {
namespace {

bool isDigit(char character) noexcept {
    return character >= '0' && character <= '9';
}

template <typename T>
Value numberFrom(T value) {
    std::array<char, 32> buffer{};
    const std::to_chars_result result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return Value::number(std::string_view(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())));
}

const std::string& emptyString() noexcept {
    static const std::string kEmpty;
    return kEmpty;
}

} // namespace

bool isJsonNumber(std::string_view text) noexcept {
    std::size_t at = 0;
    const auto peek = [&]() noexcept { return at < text.size() ? text[at] : '\0'; };
    const auto digits = [&]() noexcept {
        const std::size_t start = at;
        while (isDigit(peek())) {
            ++at;
        }
        return at > start;
    };
    if (peek() == '-') {
        ++at;
    }
    if (peek() == '0') {
        ++at;
    } else if (!digits()) {
        return false;
    }
    if (peek() == '.') {
        ++at;
        if (!digits()) {
            return false;
        }
    }
    if (peek() == 'e' || peek() == 'E') {
        ++at;
        if (peek() == '+' || peek() == '-') {
            ++at;
        }
        if (!digits()) {
            return false;
        }
    }
    return at == text.size();
}

Value Value::boolean(bool value) {
    Value result;
    result.m_kind = Kind::Bool;
    result.m_bool = value;
    return result;
}

Value Value::number(std::string_view text) {
    const bool valid = isJsonNumber(text);
    ITH_ASSERT_MSG(valid, "Value::number: not a JSON number");
    Value result;
    result.m_kind = Kind::Number;
    result.m_text = valid ? std::string(text) : std::string("0");
    return result;
}

Value Value::integer(std::int64_t value) {
    return numberFrom(value);
}

Value Value::unsignedInteger(std::uint64_t value) {
    return numberFrom(value);
}

Value Value::real(double value) {
    const bool finite = std::isfinite(value);
    ITH_ASSERT_MSG(finite, "Value::real: JSON has no infinities or NaN");
    return numberFrom(finite ? value : 0.0);
}

Value Value::string(std::string text) {
    Value result;
    result.m_kind = Kind::String;
    result.m_text = std::move(text);
    return result;
}

Value Value::array() {
    Value result;
    result.m_kind = Kind::Array;
    return result;
}

Value Value::array(std::vector<Value> elements) {
    Value result;
    result.m_kind = Kind::Array;
    result.m_elements = std::move(elements);
    return result;
}

Value Value::object() {
    Value result;
    result.m_kind = Kind::Object;
    return result;
}

bool Value::asBool() const noexcept {
    ITH_ASSERT_MSG(m_kind == Kind::Bool, "Value::asBool: not a Bool");
    return m_kind == Kind::Bool && m_bool;
}

std::string_view Value::numberText() const noexcept {
    ITH_ASSERT_MSG(m_kind == Kind::Number, "Value::numberText: not a Number");
    return m_kind == Kind::Number ? std::string_view(m_text) : std::string_view();
}

const std::string& Value::asString() const noexcept {
    ITH_ASSERT_MSG(m_kind == Kind::String, "Value::asString: not a String");
    return m_kind == Kind::String ? m_text : emptyString();
}

std::vector<Value>& Value::elements() noexcept {
    ITH_ASSERT_MSG(m_kind == Kind::Array, "Value::elements: not an Array");
    return m_elements;
}

const std::vector<Value>& Value::elements() const noexcept {
    ITH_ASSERT_MSG(m_kind == Kind::Array, "Value::elements: not an Array");
    return m_elements;
}

std::vector<Value::Member>& Value::members() noexcept {
    ITH_ASSERT_MSG(m_kind == Kind::Object, "Value::members: not an Object");
    return m_members;
}

const std::vector<Value::Member>& Value::members() const noexcept {
    ITH_ASSERT_MSG(m_kind == Kind::Object, "Value::members: not an Object");
    return m_members;
}

Value* Value::find(std::string_view key) noexcept {
    return const_cast<Value*>(std::as_const(*this).find(key));
}

const Value* Value::find(std::string_view key) const noexcept {
    if (m_kind != Kind::Object) {
        return nullptr;
    }
    for (const Member& member : m_members) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

Value& Value::set(std::string key, Value value) {
    ITH_ASSERT_MSG(m_kind == Kind::Object, "Value::set: not an Object");
    for (Member& member : m_members) {
        if (member.key == key) {
            member.value = std::move(value);
            return member.value;
        }
    }
    m_members.push_back(Member{.key = std::move(key), .value = std::move(value)});
    return m_members.back().value;
}

bool Value::remove(std::string_view key) {
    ITH_ASSERT_MSG(m_kind == Kind::Object, "Value::remove: not an Object");
    return std::erase_if(m_members, [key](const Member& member) { return member.key == key; }) != 0;
}

bool operator==(const Value& a, const Value& b) {
    if (a.m_kind != b.m_kind) {
        return false;
    }
    switch (a.m_kind) {
        case Value::Kind::Null:
            return true;
        case Value::Kind::Bool:
            return a.m_bool == b.m_bool;
        case Value::Kind::Number:
        case Value::Kind::String:
            return a.m_text == b.m_text;
        case Value::Kind::Array:
            return a.m_elements == b.m_elements;
        case Value::Kind::Object:
            return a.m_members == b.m_members;
    }
    return false;
}

} // namespace ith::serialization
