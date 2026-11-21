#pragma once

// Expected<T, E>: either a value or an error (ADR-0001).
//
// The engine is built without exceptions, so functions that can fail for reasons outside
// the program's control (I/O, parsing, user data, the network) return Expected. Reading the
// value of an Expected that holds an error is a programmer error and asserts.
//
//   ith::Expected<Config> loadConfig(std::string_view path);
//
//   auto config = loadConfig("game.ini");
//   if (!config) {
//       log(config.error().message);
//       return;
//   }
//   use(*config);
//
// It mirrors the parts of C++23 std::expected the engine needs, so it can become an alias
// once every target toolchain ships std::expected.

#include "ith/foundation/Assert.h"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ith {

// A general-purpose error: a stable code for programs and a message for people.
struct Error {
    std::string code;    // Dotted, stable identifier, e.g. "io.not_found". Never localized.
    std::string message; // Human-readable explanation for logs and tools.

    friend bool operator==(const Error&, const Error&) = default;
};

// Wraps an error value so it can initialize an Expected unambiguously.
template <typename E>
class Unexpected {
public:
    explicit Unexpected(E error) : m_error(std::move(error)) {}

    E& error() & { return m_error; }

    const E& error() const& { return m_error; }

    E&& error() && { return std::move(m_error); }

private:
    E m_error;
};

template <typename E>
Unexpected<std::decay_t<E>> makeUnexpected(E&& error) {
    return Unexpected<std::decay_t<E>>(std::forward<E>(error));
}

inline Unexpected<Error> makeError(std::string code, std::string message) {
    return Unexpected<Error>(Error{.code = std::move(code), .message = std::move(message)});
}

namespace detail {
template <typename E>
struct ErrorBox {
    E error;
};
} // namespace detail

template <typename T, typename E = Error>
class [[nodiscard]] Expected {
    static_assert(!std::is_reference_v<T>, "Expected does not hold references; use a pointer or handle.");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Unexpected<E>>);

public:
    using ValueType = T;
    using ErrorType = E;

    Expected()
        requires std::is_default_constructible_v<T>
        : m_storage(std::in_place_index<0>) {}

    template <typename U = T>
        requires std::is_constructible_v<T, U&&> && (!std::is_same_v<std::remove_cvref_t<U>, Expected>) &&
                 (!std::is_same_v<std::remove_cvref_t<U>, Unexpected<E>>)
    Expected(U&& value) // NOLINT(misc-explicit-constructor): implicit like std::expected
        : m_storage(std::in_place_index<0>, std::forward<U>(value)) {}

    template <typename G>
        requires std::is_constructible_v<E, const G&>
    Expected(const Unexpected<G>& error) // NOLINT(misc-explicit-constructor)
        : m_storage(std::in_place_index<1>, detail::ErrorBox<E>{E(error.error())}) {}

    template <typename G>
        requires std::is_constructible_v<E, G&&>
    Expected(Unexpected<G>&& error) // NOLINT(misc-explicit-constructor)
        : m_storage(std::in_place_index<1>, detail::ErrorBox<E>{E(std::move(error).error())}) {}

    bool hasValue() const { return m_storage.index() == 0; }

    explicit operator bool() const { return hasValue(); }

    T& value() & { return *valuePointer(); }

    const T& value() const& { return *valuePointer(); }

    T&& value() && { return std::move(*valuePointer()); }

    T& operator*() & { return value(); }

    const T& operator*() const& { return value(); }

    T&& operator*() && { return std::move(*this).value(); }

    T* operator->() { return valuePointer(); }

    const T* operator->() const { return valuePointer(); }

    E& error() & { return errorBox()->error; }

    const E& error() const& { return errorBox()->error; }

    E&& error() && { return std::move(errorBox()->error); }

    template <typename U>
    T valueOr(U&& fallback) const& {
        return hasValue() ? *valuePointer() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    T* valuePointer() {
        T* value = std::get_if<0>(&m_storage);
        ITH_ASSERT_MSG(value != nullptr, "Expected holds an error, not a value");
        return value;
    }

    const T* valuePointer() const {
        const T* value = std::get_if<0>(&m_storage);
        ITH_ASSERT_MSG(value != nullptr, "Expected holds an error, not a value");
        return value;
    }

    detail::ErrorBox<E>* errorBox() {
        auto* box = std::get_if<1>(&m_storage);
        ITH_ASSERT_MSG(box != nullptr, "Expected holds a value, not an error");
        return box;
    }

    const detail::ErrorBox<E>* errorBox() const {
        const auto* box = std::get_if<1>(&m_storage);
        ITH_ASSERT_MSG(box != nullptr, "Expected holds a value, not an error");
        return box;
    }

    // The error is boxed so that T and E may be the same type.
    std::variant<T, detail::ErrorBox<E>> m_storage;
};

// Expected<void, E>: success without a value, or an error.
template <typename E>
class [[nodiscard]] Expected<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    Expected() = default;

    template <typename G>
        requires std::is_constructible_v<E, const G&>
    Expected(const Unexpected<G>& error) // NOLINT(misc-explicit-constructor)
        : m_error(std::in_place_index<1>, E(error.error())) {}

    template <typename G>
        requires std::is_constructible_v<E, G&&>
    Expected(Unexpected<G>&& error) // NOLINT(misc-explicit-constructor)
        : m_error(std::in_place_index<1>, E(std::move(error).error())) {}

    bool hasValue() const { return m_error.index() == 0; }

    explicit operator bool() const { return hasValue(); }

    // Asserts that the operation succeeded.
    void value() const { ITH_ASSERT_MSG(hasValue(), "Expected holds an error"); }

    E& error() & { return *errorPointer(); }

    const E& error() const& { return *errorPointer(); }

    E&& error() && { return std::move(*errorPointer()); }

private:
    E* errorPointer() {
        E* error = std::get_if<1>(&m_error);
        ITH_ASSERT_MSG(error != nullptr, "Expected holds success, not an error");
        return error;
    }

    const E* errorPointer() const {
        const E* error = std::get_if<1>(&m_error);
        ITH_ASSERT_MSG(error != nullptr, "Expected holds success, not an error");
        return error;
    }

    std::variant<std::monostate, E> m_error;
};

} // namespace ith
