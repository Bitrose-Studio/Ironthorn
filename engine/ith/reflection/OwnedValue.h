#pragma once

// OwnedValue: a value of any reflected type in its own aligned memory (ADR-0033).
//
//   ith::OwnedValue health = ith::OwnedValue::of(game::Health{.current = 50.0f});
//   ith::OwnedValue copy = ith::OwnedValue::copyOf(ith::typeOf<game::Health>(), &someHealth);
//   if (const game::Health* value = health.as<game::Health>()) { ... }
//
// Type-erased code (patches, archives, the editor) holds values of types it knows only by their
// TypeInfo in one. The value is built, copied, moved and destroyed through the type's records.
// An OwnedValue is either empty or holds one value; a moved-from OwnedValue is empty.
//
// Not thread-safe; distinct OwnedValues may be used from different threads.

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace ith {

class OwnedValue {
public:
    // An empty OwnedValue.
    OwnedValue() noexcept = default;

    // A default-constructed value of `type`.
    explicit OwnedValue(const TypeInfo& type);

    // A copy of the value of `type` at `source`, which is a complete object of that type.
    [[nodiscard]] static OwnedValue copyOf(const TypeInfo& type, const void* source);

    // A value of the reflected type T, moved from `value`.
    template <typename T>
    [[nodiscard]] static OwnedValue of(T value);

    // Copies the held value through its type's records; a copy of an empty OwnedValue is empty.
    OwnedValue(const OwnedValue& other);
    OwnedValue& operator=(const OwnedValue& other);
    OwnedValue(OwnedValue&& other) noexcept = default;
    OwnedValue& operator=(OwnedValue&& other) noexcept = default;
    ~OwnedValue() = default;

    // True when a value is held.
    [[nodiscard]] bool hasValue() const noexcept { return m_memory != nullptr; }

    // The type of the held value; null when empty.
    [[nodiscard]] const TypeInfo* type() const noexcept { return hasValue() ? m_memory.get_deleter().type : nullptr; }

    // The held value; null when empty.
    [[nodiscard]] void* get() noexcept { return m_memory.get(); }

    [[nodiscard]] const void* get() const noexcept { return m_memory.get(); }

    // The held value as T; null when empty or when the value is of another type (compared by id).
    template <typename T>
    [[nodiscard]] T* as() noexcept;
    template <typename T>
    [[nodiscard]] const T* as() const noexcept;

    // Replaces the value of the same type at `target` with the held one: destroys the target, then
    // move-constructs it from the held value, which stays a valid moved-from value. Can't fail.
    // Preconditions (asserted; nothing happens when they fail): a value is held, and `target` is a
    // complete, non-const object of its type.
    void moveInto(void* target);

private:
    // Destroys the value and frees its memory with the type's alignment.
    struct Deleter {
        const TypeInfo* type = nullptr;
        void operator()(void* memory) const noexcept;
    };

    std::unique_ptr<void, Deleter> m_memory{nullptr, Deleter{}};
};

template <typename T>
OwnedValue OwnedValue::of(T value) {
    static_assert(!std::is_const_v<T> && !std::is_reference_v<T>);
    OwnedValue result(typeOf<T>());
    *static_cast<T*>(result.get()) = std::move(value);
    return result;
}

template <typename T>
T* OwnedValue::as() noexcept {
    return hasValue() && type()->id == typeOf<std::remove_const_t<T>>().id ? static_cast<T*>(get()) : nullptr;
}

template <typename T>
const T* OwnedValue::as() const noexcept {
    return hasValue() && type()->id == typeOf<std::remove_const_t<T>>().id ? static_cast<const T*>(get()) : nullptr;
}

} // namespace ith
