# Reflection

Reflection describes the engine's data types for code that doesn't know them at compile time:
the ECS, serialization, the Details panel, replication and save games all read the same
records. The design is [ADR-0018](../decisions/0018-reflection.md), corrected by
[ADR-0019](../decisions/0019-reflection-rules-corrected.md). This page shows how to use it.

## How it works

1. You annotate types and fields in a module's headers with `ITH_TYPE`, `ITH_FIELD` and
   `ITH_ENUM` (`ith/reflection/Annotations.h`).
2. At build time, the generator `ith_reflect` (`tools/ith_reflect`) scans the module's headers
   and writes `<module>.reflect.cpp` into the build tree. The compiler works out sizes, offsets
   and field types, so a scanner mistake is a compile error, not wrong data.
3. At startup, the application calls each module's registration function, which adds its types
   to an `ith::TypeRegistry`. Nothing registers itself through static initializers.
4. Code reads the records through `ith::typeOf<T>()` or the registry.

To turn it on for a module, add `REFLECT` to its declaration:

```cmake
ith_add_module(gameplay
    LAYER framework
    SOURCES Health.h Health.cpp
    DEPENDS foundation
    REFLECT)
```

`REFLECT` adds a dependency on the `reflection` module. The generator reruns only when one of
the module's headers changes and rewrites the generated file only when its content changes.

## Annotating a type

```cpp
#include "ith/reflection/Annotations.h"

namespace game {

struct Health {
    ITH_TYPE(Component, Category = "Combat")
    ITH_FIELD(Min = 0, Max = 10000, Tooltip = "Hit points", Replicated, SaveGame)
    float current = 100.0f;
    ITH_FIELD(Min = 1, Max = 10000)
    float maximum = 100.0f;
    float cachedRatio = 1.0f; // not reflected
};

enum class DamageKind : std::uint8_t {
    Physical,
    Fire,
    Poison,
};
ITH_ENUM(DamageKind, Tooltip = "How damage is dealt")

} // namespace game
```

- `ITH_TYPE` is the first line of the body, **without a semicolon**. Its first argument is the
  role: `Struct`, `Component`, `Resource`, `Event` or `Asset`.
- **Fields are opt-in.** Only fields after an `ITH_FIELD` line are reflected; a forgotten
  `ITH_FIELD` means the field is not saved, loaded or replicated.
- `ITH_ENUM` goes on the line directly after the enum, **without a semicolon**. Its first
  argument is the enum's name, which must match the enum before it; attributes follow.
- Fields may be private; `ITH_TYPE` gives the generated code access.
- A type derived from a reflected type is **not** reflected; using it as one is a compile error.
  Reflected types compose, they don't inherit.

### Field types

- `bool`, `std::int8_t` to `std::int64_t`, `std::uint8_t` to `std::uint64_t`, `float`,
  `double`, `std::string` and `ith::Uuid`;
- reflected enums and reflected structs (composition);
- `std::array<T, N>`, `std::vector<T>` and `std::optional<T>` of these.

Any other field type is a compile error in the generated file. Handles, asset references,
gameplay tags and math types join the list as those modules arrive.

### Syntax rules

The generator reads a restricted syntax and refuses anything else with a `file:line` error:

- A reflected type is a `struct`, `class` or `enum class` at namespace scope: not nested in
  another type, not in an anonymous namespace, not a template, without base classes, and
  defined inside its namespace (not `struct game::Health {`).
- One field per `ITH_FIELD`, one declarator per line. No C arrays (use `std::array`), bit-fields,
  pointers, references, `static` or `const` fields, and no attributes (`[[no_unique_address]]`
  and the like) or `alignas` on a reflected field: a field must be destroyable and rebuildable
  in place.
- A reflected enum is an `enum class` with an enumerator list and an integer underlying type
  other than `bool`. Using an unannotated enum as a field type is a compile error.
- No annotations inside `#if` blocks, and no `#if` splitting a reflected field or enum. Reflected
  types look the same in every configuration. Headers use `#pragma once`, not include guards.
- No virtual functions in a reflected type.

## Attributes

| Attribute | On | Meaning |
|---|---|---|
| `EditorOnly` | type, field, enum | Exists only in `editor` modules; anywhere else it is a generation error ([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)). |
| `Transient` | type, field | Never saved or loaded. |
| `ReadOnly` | field | Shown but not editable in the editor. |
| `Replicated` | field | Sent over the network. |
| `SaveGame` | field | Stored in save games. |
| `Flags` | enum | The enum's values combine as bit flags. |
| `Min = <number>`, `Max = <number>` | field | Allowed range of a number. |
| `Unit = "<text>"` | field | Unit of a number, e.g. `"cm"`. |
| `Category = "<text>"` | type, field, enum | Group in the Details panel. |
| `DisplayName = "<text>"` | type, field, enum | Name shown to people. |
| `Tooltip = "<text>"` | type, field, enum | Help text. |
| `FormerName = "<name>"` | type, field, enum | The previous name (see below). |
| `Meta(Key = "value", ...)` | type, field, enum | Free key-value strings for plugins and tools. |

An unknown, duplicated or misplaced attribute is a generation error. Text values are plain
string literals.

**Shipping** builds keep names, ids, layout, flags, `Min`, `Max`, `Unit` and `FormerName`, and
compile out the human-facing text: `Category`, `DisplayName`, `Tooltip` and `Meta` are empty.

## Ids and renames

- A **type id** is `ith::fnv1a64` of the fully qualified name as written (`"game::Health"`), and
  a **field id** is `ith::fnv1a64` of the field name. Ids are the same on every compiler and
  platform and appear in data, so the hash function never changes.
- **Renaming** a type, field or enum: add `FormerName` with the old name.

  ```cpp
  struct Health {
      ITH_TYPE(Component, FormerName = "game::Hp")
      ITH_FIELD(FormerName = "hitPoints")
      float current = 100.0f;
  };
  ```

  The registry then finds the type by its old name and id too, and `TypeInfo::findField` finds
  the field by its old name, so data written under the old names still loads. Cooked data is
  recooked.
- Two types with the same id in one process are a fatal startup error naming both types. Two
  types with the same name in one module are a generation error, and so is a field's
  `FormerName` that is another field's name or another field's `FormerName`.
- `Min` and `Max` are stored as `double`, exact for integers up to 2^53.

## Reading the records

```cpp
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"

ITH_DECLARE_REFLECTION_MODULE(gameplay)

ith::TypeRegistry registry;                          // holds the leaf types
ith::generated::registerReflection_gameplay(registry); // once per module, at startup

const ith::TypeInfo& health = ith::typeOf<game::Health>();
for (const ith::FieldInfo& field : health.fields) {
    const ith::TypeInfo& fieldType = field.type();
    // field.name, field.offset, field.flags, field.attributes ...
}
const ith::TypeInfo* byName = registry.find("game::Health");
```

- `TypeInfo` has the id, name, kind, role, size, alignment, whether the type is trivially
  copyable (so storage may move it with `memcpy`), flags and attributes, and function
  pointers to construct, copy, move and destroy a value and compare two values. Structs compare
  their reflected fields only.
- Containers expose their element type (`element()`) and element access (`container`).
- Enums list their enumerators and expose `readEnumValue` and `writeEnumValue`.
- `typeOf<T>()` of an unreflected type is a compile error. An annotated type in a module
  without `REFLECT` fails at link time.
- **Compare records by id, never by address**: a container's record is built once per binary,
  so one type can have several records in a process.
- The registry is filled at startup and is read-only afterwards; lookups don't allocate. It
  holds pointers to static records, so a module's entries must be removed before the module is
  unloaded (hot reload defines when).

## Tests

- `reflect_tool` runs the generator in-process: a golden file for a fixture header, the
  accepted syntax, and every refused construct with its message and line. After a deliberate
  change to the output, the test writes the new output to `build/Basic.reflect.cpp.actual`;
  review it and copy it over `tests/reflect_tool/golden/Basic.reflect.cpp.golden`.
- `reflection` builds a test module of reflected types through the same build step as engine
  modules and checks the records, the registry and a round-trip of every type through a
  field-wise copy. The round-trip through the text and binary archives
  ([ADR-0010](../decisions/0010-serialization.md)) replaces that copy when serialization lands.
- In Clang sanitizer builds, `reflect_tool_fuzz` runs a short libFuzzer session over the
  generator (`tests/reflect_tool/ScannerFuzz.cpp`), seeded with the fixture headers.
