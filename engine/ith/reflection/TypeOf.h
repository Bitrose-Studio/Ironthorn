#pragma once

// ith::typeOf<T>(): the TypeInfo of a reflected type (ADR-0018).
//
//   const ith::TypeInfo& health = ith::typeOf<game::Health>();   // an ITH_TYPE struct
//   const ith::TypeInfo& kind = ith::typeOf<game::DamageKind>(); // an ITH_ENUM enum
//   const ith::TypeInfo& list = ith::typeOf<std::vector<float>>(); // a container
//
// T is one of:
// - a leaf type, described here by hand: bool, std::int8_t to std::int64_t, std::uint8_t to
//   std::uint64_t, float, double, std::string, ith::Uuid;
// - a struct or class annotated with ITH_TYPE, or an enum class annotated with ITH_ENUM, whose
//   record the ith_reflect generator writes into the owning module (a type derived from a
//   reflected type is not reflected);
// - std::array, std::vector or std::optional of a supported type.
//
// Any other T is a compile error. An annotated type whose module doesn't set REFLECT fails at
// link time. Compare records by id, never by address: a container's record can exist once per
// binary (ADR-0019).
//
// The returned record lives for the whole program. Calling typeOf is thread-safe and doesn't
// allocate after the first call for a container type.

#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeInfo.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ith {

template <typename T>
const TypeInfo& typeOf() noexcept;

namespace detail {

template <typename T>
inline constexpr bool kAlwaysFalse = false;

// Reaches the members ITH_TYPE declares, which may be private.
struct ReflectionAccess {
    // True only for the type that carries ITH_TYPE itself: a derived type inherits the members,
    // but its marker is the base's.
    template <typename T>
    static constexpr bool kIsReflectedStruct = requires {
        { T::ithTypeInfo() } -> std::same_as<const TypeInfo&>;
        requires std::is_same_v<decltype(&T::ithReflected), void (T::*)() const noexcept>;
    };

    template <typename T>
    static const TypeInfo& typeInfo() noexcept {
        return T::ithTypeInfo();
    }
};

// An enum annotated with ITH_ENUM: ITH_ENUM declares ithEnumTypeInfo(E*) in the enum's namespace
// and the generated file defines it.
template <typename T>
concept IsReflectedEnum = std::is_enum_v<T> && requires {
    { ithEnumTypeInfo(static_cast<T*>(nullptr)) } -> std::same_as<const TypeInfo&>;
};

// Value functions get memory from type-erased callers; check it suits T in debug builds.
template <typename T>
bool isAlignedFor(const void* memory) noexcept {
    return reinterpret_cast<std::uintptr_t>(memory) % alignof(T) == 0;
}

// The value operations every TypeInfo points to.
template <typename T>
void constructValue(void* memory) {
    ITH_ASSERT_MSG(isAlignedFor<T>(memory), "reflection: misaligned memory");
    ::new (memory) T();
}

template <typename T>
void copyValue(void* destination, const void* source) {
    ITH_ASSERT_MSG(isAlignedFor<T>(destination), "reflection: misaligned memory");
    ::new (destination) T(*static_cast<const T*>(source));
}

template <typename T>
void moveValue(void* destination, void* source) {
    ITH_ASSERT_MSG(isAlignedFor<T>(destination), "reflection: misaligned memory");
    ::new (destination) T(std::move(*static_cast<T*>(source)));
}

template <typename T>
void destroyValue(void* object) noexcept {
    static_cast<T*>(object)->~T();
}

template <typename T>
bool equalValues(const void* a, const void* b) {
    return *static_cast<const T*>(a) == *static_cast<const T*>(b);
}

template <typename T>
bool equalStructs(const void* a, const void* b) {
    return equalFields(typeOf<T>(), a, b);
}

// The parts of a record every kind shares. `equals` compares two values of T.
template <typename T>
constexpr TypeInfo makeValueType(TypeKind kind, std::string_view name,
                                 bool (*equals)(const void*, const void*)) noexcept {
    static_assert(std::is_default_constructible_v<T> && std::is_copy_constructible_v<T> &&
                      std::is_move_constructible_v<T>,
                  "reflected types must be default-constructible, copyable and movable");
    return TypeInfo{
        .id = fnv1a64(name),
        .name = name,
        .kind = kind,
        .size = sizeof(T),
        .alignment = alignof(T),
        .triviallyCopyable = std::is_trivially_copyable_v<T>,
        .construct = &constructValue<T>,
        .copy = &copyValue<T>,
        .move = &moveValue<T>,
        .destroy = &destroyValue<T>,
        .equals = equals,
    };
}

// Builds the record of an ITH_TYPE struct. Used by generated code.
template <typename T>
constexpr TypeInfo makeStructType(std::uint64_t id, std::string_view name, TypeRole role, ReflectionFlags flags,
                                  const Attributes& attributes, std::span<const FieldInfo> fields) noexcept {
    TypeInfo info = makeValueType<T>(TypeKind::Struct, name, &equalStructs<T>);
    info.id = id;
    info.role = role;
    info.flags = flags;
    info.attributes = attributes;
    info.fields = fields;
    return info;
}

// Builds the record of an ITH_ENUM enum. Used by generated code.
template <typename E>
constexpr TypeInfo makeEnumType(std::uint64_t id, std::string_view name, ReflectionFlags flags,
                                const Attributes& attributes, std::span<const EnumeratorInfo> enumerators) noexcept {
    static_assert(std::is_enum_v<E> && !std::is_convertible_v<E, std::underlying_type_t<E>>,
                  "only enum class types can be reflected");
    static_assert(!std::is_same_v<std::underlying_type_t<E>, bool>,
                  "enums with a bool underlying type can't be reflected");
    TypeInfo info = makeValueType<E>(TypeKind::Enum, name, &equalValues<E>);
    info.id = id;
    info.flags = flags;
    info.attributes = attributes;
    info.enumerators = enumerators;
    info.element = &typeOf<std::underlying_type_t<E>>;
    return info;
}

// The hand-written leaf records. TypeOfImpl<T> is specialized for every supported type that
// is not an ITH_TYPE struct or an ITH_ENUM enum.
template <typename T>
struct TypeOfImpl;

template <typename T>
concept HasTypeOfImpl = requires { TypeOfImpl<T>::get(); };

// clang-format versions disagree on how to lay out this macro, so it is left as written.
// clang-format off
#define ITH_DETAIL_LEAF_TYPE(cppType, typeKind, typeName)                                                              \
    template <>                                                                                                        \
    struct TypeOfImpl<cppType> {                                                                                       \
        static constexpr TypeInfo kInfo = makeValueType<cppType>(TypeKind::typeKind, typeName, &equalValues<cppType>); \
        static const TypeInfo& get() noexcept {                                                                        \
            return kInfo;                                                                                              \
        }                                                                                                              \
    }
// clang-format on

ITH_DETAIL_LEAF_TYPE(bool, Bool, "bool");
ITH_DETAIL_LEAF_TYPE(std::int8_t, SignedInteger, "int8");
ITH_DETAIL_LEAF_TYPE(std::int16_t, SignedInteger, "int16");
ITH_DETAIL_LEAF_TYPE(std::int32_t, SignedInteger, "int32");
ITH_DETAIL_LEAF_TYPE(std::int64_t, SignedInteger, "int64");
ITH_DETAIL_LEAF_TYPE(std::uint8_t, UnsignedInteger, "uint8");
ITH_DETAIL_LEAF_TYPE(std::uint16_t, UnsignedInteger, "uint16");
ITH_DETAIL_LEAF_TYPE(std::uint32_t, UnsignedInteger, "uint32");
ITH_DETAIL_LEAF_TYPE(std::uint64_t, UnsignedInteger, "uint64");
ITH_DETAIL_LEAF_TYPE(float, Float, "float");
ITH_DETAIL_LEAF_TYPE(double, Float, "double");
ITH_DETAIL_LEAF_TYPE(std::string, String, "std::string");
ITH_DETAIL_LEAF_TYPE(Uuid, Uuid, "ith::Uuid");

#undef ITH_DETAIL_LEAF_TYPE

// The leaf records, for registering them with a TypeRegistry.
inline constexpr std::array<const TypeInfo*, 13> kLeafTypes = {
    &TypeOfImpl<bool>::kInfo,          &TypeOfImpl<std::int8_t>::kInfo,   &TypeOfImpl<std::int16_t>::kInfo,
    &TypeOfImpl<std::int32_t>::kInfo,  &TypeOfImpl<std::int64_t>::kInfo,  &TypeOfImpl<std::uint8_t>::kInfo,
    &TypeOfImpl<std::uint16_t>::kInfo, &TypeOfImpl<std::uint32_t>::kInfo, &TypeOfImpl<std::uint64_t>::kInfo,
    &TypeOfImpl<float>::kInfo,         &TypeOfImpl<double>::kInfo,        &TypeOfImpl<std::string>::kInfo,
    &TypeOfImpl<Uuid>::kInfo,
};

// Containers. Their records are built on first use, because the id depends on the element's.
template <typename C>
bool equalElements(const void* a, const void* b) {
    const TypeInfo& type = typeOf<C>();
    const std::size_t count = type.container->size(a);
    if (count != type.container->size(b)) {
        return false;
    }
    const TypeInfo& elementType = type.element();
    for (std::size_t index = 0; index < count; ++index) {
        if (!elementType.equals(type.container->constAt(a, index), type.container->constAt(b, index))) {
            return false;
        }
    }
    return true;
}

template <typename C>
inline constexpr bool kIsOptional = requires(const C& container) { container.has_value(); };

template <typename C>
std::size_t containerSize(const void* container) noexcept {
    const C& value = *static_cast<const C*>(container);
    if constexpr (kIsOptional<C>) {
        return value.has_value() ? 1 : 0;
    } else {
        return value.size();
    }
}

template <typename C>
const void* constElementAt(const void* container, std::size_t index) noexcept {
    const bool inRange = index < containerSize<C>(container);
    ITH_ASSERT_MSG(inRange, "reflected container: element index out of range");
    if (!inRange) {
        return nullptr;
    }
    const C& value = *static_cast<const C*>(container);
    if constexpr (kIsOptional<C>) {
        return &*value;
    } else {
        return &value[index];
    }
}

template <typename C>
void* elementAt(void* container, std::size_t index) noexcept {
    return const_cast<void*>(constElementAt<C>(container, index));
}

template <typename C>
void resizeContainer(void* container, std::size_t size) {
    C& value = *static_cast<C*>(container);
    if constexpr (kIsOptional<C>) {
        ITH_ASSERT_MSG(size <= 1, "reflected std::optional: the size must be 0 or 1");
        if (size == 0) {
            value.reset();
        } else if (!value.has_value()) {
            value.emplace();
        }
    } else {
        value.resize(size);
    }
}

// Fixed arrays have no resize function.
template <typename C>
constexpr auto resizeFunction() noexcept -> void (*)(void*, std::size_t) {
    if constexpr (kIsOptional<C> || requires(C& container) { container.resize(0); }) {
        return &resizeContainer<C>;
    } else {
        return nullptr;
    }
}

template <typename C>
inline constexpr ContainerOps kContainerOps = {
    .size = &containerSize<C>,
    .at = &elementAt<C>,
    .constAt = &constElementAt<C>,
    .resize = resizeFunction<C>(),
};

// `extra` tells containers with the same element type apart, e.g. the size of a std::array.
template <typename C, typename Element>
TypeInfo makeContainerType(TypeKind kind, std::string_view name, std::uint64_t extra) noexcept {
    TypeInfo info = makeValueType<C>(kind, name, &equalElements<C>);
    info.id = hashCombine(hashCombine(fnv1a64(name), typeOf<Element>().id), extra);
    info.element = &typeOf<Element>;
    info.container = &kContainerOps<C>;
    return info;
}

template <typename T, std::size_t N>
struct TypeOfImpl<std::array<T, N>> {
    static const TypeInfo& get() noexcept {
        static const TypeInfo kInfo = makeContainerType<std::array<T, N>, T>(TypeKind::FixedArray, "std::array", N);
        return kInfo;
    }
};

template <typename T>
struct TypeOfImpl<std::vector<T>> {
    static_assert(!std::is_same_v<T, bool>,
                  "std::vector<bool> has no addressable elements; use std::vector<std::uint8_t>");

    static const TypeInfo& get() noexcept {
        static const TypeInfo kInfo = makeContainerType<std::vector<T>, T>(TypeKind::DynamicArray, "std::vector", 0);
        return kInfo;
    }
};

template <typename T>
struct TypeOfImpl<std::optional<T>> {
    static const TypeInfo& get() noexcept {
        static const TypeInfo kInfo = makeContainerType<std::optional<T>, T>(TypeKind::Optional, "std::optional", 0);
        return kInfo;
    }
};

} // namespace detail

// The record of T; see the top of this file for the supported types.
template <typename T>
const TypeInfo& typeOf() noexcept {
    if constexpr (detail::ReflectionAccess::kIsReflectedStruct<T>) {
        return detail::ReflectionAccess::typeInfo<T>();
    } else if constexpr (detail::IsReflectedEnum<T>) {
        return ithEnumTypeInfo(static_cast<T*>(nullptr));
    } else if constexpr (detail::HasTypeOfImpl<T>) {
        return detail::TypeOfImpl<T>::get();
    } else {
        static_assert(detail::kAlwaysFalse<T>,
                      "typeOf<T>: T is not reflected. Annotate it with ITH_TYPE or ITH_ENUM, or use a supported "
                      "leaf or container type (ith/reflection/TypeOf.h).");
    }
}

} // namespace ith

// Human-facing text in generated records: kept in Debug and Development, compiled out in
// Shipping (ADR-0018 rule 11).
#ifdef ITH_SHIPPING
#define ITH_REFLECTION_TEXT(text) ::std::string_view()
#define ITH_REFLECTION_META(entries) ::std::span<const ::ith::MetaEntry>()
#else
#define ITH_REFLECTION_TEXT(text) ::std::string_view(text)
#define ITH_REFLECTION_META(entries) ::std::span<const ::ith::MetaEntry>(entries)
#endif
