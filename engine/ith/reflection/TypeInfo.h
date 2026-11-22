#pragma once

// The records that describe reflected types (ADR-0018).
//
// A TypeInfo describes one type for type-erased code: its stable id and name, its size and
// alignment, how to construct, copy, move, destroy and compare a value, its flags and
// attributes, and its reflected fields (structs), enumerators (enums) or element type
// (containers). A FieldInfo describes one reflected field of a struct.
//
//   const ith::TypeInfo& type = ith::typeOf<game::Health>();
//   for (const ith::FieldInfo& field : type.fields) {
//       const ith::TypeInfo& fieldType = field.type();
//       void* value = static_cast<std::byte*>(object) + field.offset;
//       ...
//   }
//
// Records are immutable static data, written by the ith_reflect generator for annotated types
// and by hand for the few leaf and math types (TypeOf.h). They live for the whole program, so pointers
// and references to them never dangle, and reading them is thread-safe.
// Compare records by id, never by address: the record of a container type is built once per
// binary, so one type can have several records in a process (ADR-0019).
//
// Ids: a type id is ith::fnv1a64 of the fully qualified name as written ("game::Health"), and a
// field id is ith::fnv1a64 of the field name. Both are stable across compilers, platforms and
// engine versions and may be stored in data.
//
// Shipping builds keep names, ids, layout, flags, Min, Max, Unit and FormerName, and compile out
// the human-facing text: Category, DisplayName, Tooltip and Meta are empty there.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ith {

struct TypeInfo;

// What kind of type a TypeInfo describes. Leaf kinds come first.
enum class TypeKind : std::uint8_t {
    Bool,
    SignedInteger,   // int8 to int64; the size tells which
    UnsignedInteger, // uint8 to uint64
    Float,           // float or double; the size tells which
    String,          // std::string (UTF-8)
    Uuid,            // ith::Uuid
    Enum,            // an ITH_ENUM enum class
    Struct,          // an ITH_TYPE struct or class
    FixedArray,      // std::array<T, N>
    DynamicArray,    // std::vector<T>
    Optional,        // std::optional<T>, a container of zero or one element
    // ith::Entity, a runtime handle (ADR-0032). Last, so that the values of the kinds before it,
    // which binary schema hashes cover, never change.
    Entity,
};

// The role an ITH_TYPE type plays. Every other kind of type has the role None.
enum class TypeRole : std::uint8_t {
    None,
    Struct,    // plain data, used inside other types
    Component, // stored on entities (ADR-0014)
    Resource,  // a typed singleton of a world
    Event,     // sent between systems
    Asset,     // loaded from an asset file
};

// Flags of types and fields, from the annotations of the same names. A set is a bitwise OR.
enum class ReflectionFlags : std::uint8_t {
    None = 0,
    EditorOnly = 1u << 0, // exists only in editor modules (ADR-0017)
    Transient = 1u << 1,  // never saved or loaded
    ReadOnly = 1u << 2,   // shown but not editable in the editor
    Replicated = 1u << 3, // sent over the network
    SaveGame = 1u << 4,   // stored in save games
    Flags = 1u << 5,      // an enum whose values combine as bit flags
    Immutable = 1u << 6,  // a component type changed only by replacing its value (ADR-0030)
};

constexpr ReflectionFlags operator|(ReflectionFlags a, ReflectionFlags b) noexcept {
    return static_cast<ReflectionFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr ReflectionFlags operator&(ReflectionFlags a, ReflectionFlags b) noexcept {
    return static_cast<ReflectionFlags>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

// True when `set` contains every flag of `flags`.
constexpr bool hasFlags(ReflectionFlags set, ReflectionFlags flags) noexcept {
    return (set & flags) == flags;
}

// A free-form key and value from Meta(...), for plugins and tools. Empty in Shipping.
struct MetaEntry {
    std::string_view key;
    std::string_view value;
};

// The value attributes of a type or field. Absent values are empty or nullopt.
struct Attributes {
    std::string_view category;    // Details panel group. Empty in Shipping.
    std::string_view displayName; // Name shown to people. Empty in Shipping.
    std::string_view tooltip;     // Help text. Empty in Shipping.
    std::string_view unit;        // Unit of a number, e.g. "cm" or "s".
    std::string_view formerName;  // The previous name, kept as an alias after a rename.
    // Range of a number. Stored as double: exact for integers up to 2^53, so a 64-bit limit
    // beyond that is rounded.
    std::optional<double> min; // Smallest allowed value.
    std::optional<double> max; // Largest allowed value.
    std::span<const MetaEntry> meta;
};

// One reflected field of a struct.
struct FieldInfo {
    std::uint64_t id = 0;  // fnv1a64(name)
    std::string_view name; // as declared, e.g. "current"
    std::size_t offset = 0;
    const TypeInfo& (*type)() noexcept = nullptr; // the field's type; call as field.type()
    ReflectionFlags flags = ReflectionFlags::None;
    Attributes attributes;
};

// One enumerator of a reflected enum, with its value converted to 64 bits.
struct EnumeratorInfo {
    std::string_view name;
    std::int64_t value = 0;
};

// Element access for FixedArray, DynamicArray and Optional types. An optional is a container
// of zero or one element.
struct ContainerOps {
    // Number of elements.
    std::size_t (*size)(const void* container) noexcept = nullptr;
    // The element at `index`. Precondition (asserted): index < size(container); when it isn't,
    // the result is null.
    void* (*at)(void* container, std::size_t index) noexcept = nullptr;
    const void* (*constAt)(const void* container, std::size_t index) noexcept = nullptr;
    // Sets the number of elements; new elements are default-constructed. Null for fixed arrays.
    // An optional accepts 0 (reset) and 1 (keeps its value, or default-constructs one); any
    // other size asserts.
    void (*resize)(void* container, std::size_t size) = nullptr;
};

// The description of one type. All function pointers are set for every kind.
struct TypeInfo {
    std::uint64_t id = 0;  // fnv1a64(name); for containers, derived from the element type
    std::string_view name; // fully qualified, e.g. "game::Health"; "std::vector" for containers
    TypeKind kind = TypeKind::Struct;
    TypeRole role = TypeRole::None;
    ReflectionFlags flags = ReflectionFlags::None;
    std::size_t size = 0;
    std::size_t alignment = 0;
    // True when values can be copied with memcpy and need no destructor call
    // (std::is_trivially_copyable), so type-erased storage can move them without calling `move`
    // and `destroy`.
    bool triviallyCopyable = false;
    // True when a value of the type may hold an ith::Entity: the type is Entity, or a container
    // whose element type has the flag, or a reflected struct with a reflected field that may reach
    // one (ADR-0032). False guarantees that the value holds none, so ith::remapEntities returns at
    // once. A struct field whose type is another ITH_TYPE struct counts as reaching an Entity,
    // because its record lives in another translation unit and can't be read at compile time;
    // the walker then visits it and finds out. Hand-written records leave it false.
    bool containsEntities = false;

    // Default-constructs a value in uninitialized, suitably aligned memory.
    void (*construct)(void* memory) = nullptr;
    // Copy-constructs a value from `source` in uninitialized memory at `destination`.
    void (*copy)(void* destination, const void* source) = nullptr;
    // Move-constructs a value from `source` in uninitialized memory at `destination`. The
    // source stays a valid value that must still be destroyed.
    void (*move)(void* destination, void* source) = nullptr;
    // Destroys a value; its memory is uninitialized afterwards.
    void (*destroy)(void* object) noexcept = nullptr;
    // Compares two values. Structs compare their reflected fields only.
    bool (*equals)(const void* a, const void* b) = nullptr;

    Attributes attributes;
    std::span<const FieldInfo> fields;           // Struct: the reflected fields in declaration order
    std::span<const EnumeratorInfo> enumerators; // Enum: the enumerators in declaration order
    // Containers: the element type. Enums: the underlying integer type. Null otherwise.
    const TypeInfo& (*element)() noexcept = nullptr;
    const ContainerOps* container = nullptr; // Containers only.

    // The field with this name, or with this FormerName; null when there is none.
    [[nodiscard]] const FieldInfo* findField(std::string_view fieldName) const noexcept;

    // The field with this id, or whose FormerName has this id; null when there is none.
    [[nodiscard]] const FieldInfo* findField(std::uint64_t fieldId) const noexcept;

    // The enumerator with this name; null when there is none or this is not an enum.
    [[nodiscard]] const EnumeratorInfo* findEnumerator(std::string_view enumeratorName) const noexcept;
};

// Reads the value of an enum object as a 64-bit integer. Precondition (asserted): `enumType`
// is an Enum.
std::int64_t readEnumValue(const TypeInfo& enumType, const void* object) noexcept;

// Stores `value`, truncated to the enum's underlying type, in an enum object. Precondition
// (asserted): `enumType` is an Enum.
void writeEnumValue(const TypeInfo& enumType, void* object, std::int64_t value) noexcept;

namespace detail {

// Field-wise equality over the reflected fields of a struct type.
bool equalFields(const TypeInfo& type, const void* a, const void* b);

} // namespace detail
} // namespace ith
