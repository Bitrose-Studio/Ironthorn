# Serialization

The `serialization` module saves and loads any reflected value. Source data (assets, scenes,
config) is JSON text; cooked data is a compact binary blob. Both are driven by the same
reflection records, so a type is described once. The design is
[ADR-0010](../decisions/0010-serialization.md), completed by
[ADR-0020](../decisions/0020-serialization-rules-completed.md); this page shows how to use it.

## Formats and versions

Every document names its format and version. Declare them once per kind of document:

```cpp
#include "ith/serialization/TextArchive.h"

constexpr ith::serialization::DocumentFormat kLoadoutFormat{.name = "game.loadout", .version = 1};
```

- `name` is a stable identifier, 1 to 255 bytes. A reader refuses a document of another format.
- `version` is the current version; writers always write it. A document whose version is newer
  than the reader's is refused with an error that names both versions.
- `minimumVersion` and `migrations` describe how older **text** documents are upgraded (see
  [Migrations](#migrations)). There must be exactly `version - minimumVersion` migrations, so a
  format without migrations sets `minimumVersion` equal to `version`.

## Text: JSON

```cpp
const auto text = ith::serialization::writeText(kLoadoutFormat, loadout); // Expected<std::string>
if (!text) {
    log(text.error().toString());
}

std::vector<ith::serialization::Error> warnings;
const auto read = ith::serialization::readText(*text, kLoadoutFormat, loadout,
                                               {.source = "loadout.json", .warnings = &warnings});
```

A document looks like this:

```json
{
  "format": "game.loadout",
  "version": 1,
  "data": {
    "slots": [
      "Fire",
      "Poison"
    ],
    "health": {
      "current": 12.5,
      "maximum": 100
    }
  }
}
```

- Two-space indentation, LF line endings and a newline at the end, so files diff cleanly.
- Fields appear in declaration order. `Transient` fields are never written or read.
  `EditorOnly` fields are written: text is source data, which keeps what the editor needs.
  `SaveGame` has no effect yet; the save-system ADR gives it a meaning.
- Integers are exact, 64-bit included. `float` and `double` are written with `std::to_chars`
  in their shortest form and read back bit-identical, `-0` and subnormals included. Infinities
  and NaN are an error (`serialization.non_finite`): JSON has no such numbers.
- `ith::Uuid` is its canonical string. An enum is its enumerator's name; a `Flags` enum is an
  array of names whose values are ORed (`[]` for 0). A value without an enumerator is an error.
- `std::array` and `std::vector` are arrays, `std::optional` is `null` or its value. An optional
  of an optional is refused, because JSON can't tell its two empty states apart.

### Reading rules

- **Strict values.** A value of the wrong JSON type, an integer with a fraction or out of its
  type's range, an unknown enumerator or a malformed UUID fails the read.
- **Lenient fields.** A missing field keeps its default value. A field may be named by its
  `FormerName`; giving both names, or one name twice, is `serialization.duplicate_field`.
- **Unknown fields** are warnings by default: the read succeeds, the field is dropped and the
  warning names it (`$.where.bogus`). Warnings are collected only when `warnings` is set, at
  most 100 of them plus one that counts the rest. Cooking passes `UnknownFields::Fail`, which turns them into
  errors so stale data never ships.
- **Comments** (`//` and `/* */`) are allowed only with `JsonSyntax::Config`, for hand-edited
  config files. Assets and scenes are strict RFC 8259 JSON.
- **Limits:** documents up to `kMaxTextSize` (256 MiB, or the lower `maxSize` a caller sets; config
  files need far less) and nesting up to `kMaxDepth` (64) arrays and objects, the document's own
  object included.
- **Allocation budget.** A read allocates at most `allocationBudget` bytes, by default the larger
  of 64 MiB and 16 times the input size (`Document.h`). The JSON tree is checked against it before
  it is built, and every container and string before it is resized, so a small file can't make
  the reader allocate gigabytes; exceeding it is `serialization.too_large`. Callers that load
  very large documents pass a larger budget.
- **Validate, then apply.** A read builds the whole value in a temporary object and only then
  moves it into the target. A failed read leaves the target untouched. A successful read replaces
  the whole object: `Transient` and unreflected members get their default values, and a vector
  or optional holds exactly what the document lists, not its default contents.

### Asset documents

A native asset is a document with its asset id between `version` and `data`
([ADR-0024](../decisions/0024-asset-document-envelope-and-extension.md)):

```cpp
const ith::Uuid id = ...; // the asset's id
const auto text = ith::serialization::writeText(ith::kInputContextFormat, context, &id);

ith::Uuid readId;
const auto read = ith::serialization::readText(*text, ith::kInputContextFormat, context, {.id = &readId});
```

- Whether a document must have an id is the reader's choice. With `TextReadOptions::id` set, a
  document without an `id`, or with one that isn't a UUID other than nil, is refused; without it,
  a document that has an `id` is refused. Both are `serialization.not_a_document`, whatever the
  unknown-field mode says. Writing a nil id is `serialization.invalid_id`.
- Migrations see `data` only; they never change `format`, `version` or `id`.

### Documents keyed by names

Some documents are not a reflected type: configuration keyed by names the engine doesn't know in
advance, such as the input bindings (`ith.input_bindings`). `readDocument` checks the envelope like
`readText`, runs the migrations and returns the `data` tree as a `Value`; `writeDocument` writes a
tree as a document, with an id for an asset document.

### Migrations

When a type changes in a way old files can't express (a field is renamed without `FormerName`,
removed, or changes meaning or type), bump the format's version and add a migration. A
migration edits the document's data as a `Value` tree before reflection reads it:

```cpp
// Version 1 called the field "hp"; version 2 calls it "current".
ith::Expected<void, ith::serialization::Error> renameHp(ith::serialization::Value& data) {
    if (ith::serialization::Value* hp = data.find("hp")) {
        ith::serialization::Value moved = std::move(*hp);
        data.remove("hp");
        data.set("current", std::move(moved));
    }
    return {};
}

constexpr ith::serialization::Migration kHealthMigrations[] = {&renameHp};
constexpr ith::serialization::DocumentFormat kHealthFormat{
    .name = "game.health", .version = 2, .minimumVersion = 1, .migrations = kHealthMigrations};
```

- Migrations run in order, version N to N + 1, until the current version. If one fails, the
  read fails with `serialization.migration_failed` and the target is untouched.
- Files are rewritten in the new version only when they are saved.
- Adding a field with a default needs no new version: a missing field takes its default.
- Before a major release, old migrations are removed and `minimumVersion` is raised; older files
  are then refused with an error that names the minimum.

### Values inside other documents

`toValue` and `fromValue` (`ith/serialization/TextArchive.h`) convert one reflected value to and
from a `Value` tree with the text archive's rules, but without a document around it. Use them for
values that are kept inside other data, such as prefab overrides and editor command arguments
([ADR-0033](../decisions/0033-reflection-property-paths-and-patches.md)):

```cpp
const auto tree = ith::serialization::toValue(ith::typeOf<game::Health>(), &health);
game::Health read;
const auto result = ith::serialization::fromValue(*tree, ith::typeOf<game::Health>(), &read);
```

- Error paths start at `$`, the value itself.
- `fromValue` takes `ValueReadOptions`: the unknown-field mode, a warnings list, the allocation
  budget (by default `kMinAllocationBudget`) and the entity mapping. It builds the whole value
  first, so a failed read leaves the target unchanged.
- Reading many values from one document, pass `sharedBudget`, a pointer to the bytes left of one
  budget for all of them: each read draws from it, so the document as a whole stays within its
  budget instead of each value getting a budget of its own.
- `serialization::printable(text)` (`Error.h`) makes names and text from the data safe for
  messages: at most 64 bytes, other bytes escaped.
- `Entity` values need an `EntityMapping`, as in documents.

### Free-form JSON

`ith/serialization/Json.h` reads and writes `Value` trees directly, for data without a reflected
type such as editor commands. `Value` keeps number tokens as text, so `toNumber<T>()` converts
them exactly or reports that they don't fit.

## Binary: cooked data

```cpp
#include "ith/serialization/BinaryArchive.h"

const auto blob = ith::serialization::writeBinary(kLoadoutFormat, loadout); // Expected<std::vector<std::byte>>
const auto read = ith::serialization::readBinary(*blob, kLoadoutFormat, loadout, {.source = "loadout.bin"});
```

- A blob is a header (magic `ITHB`, format version, schema hash, payload size, format name,
  padded to 16 bytes) followed by the value, little-endian and packed. The layout is documented
  in `BinaryArchive.h`.
- Floats are stored as their bits, so every value reads back bit-identical, NaN included.
- Strings are stored as bytes and are not checked for UTF-8.
- **Cooked data has no editor data.** `Transient` and `EditorOnly` fields, and fields of
  `EditorOnly` types, are left out; writing an `EditorOnly` value itself is an error
  ([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)).
- **Cooked data isn't migrated.** A blob of another format version, or whose schema hash (a hash
  of the ids, kinds, encoded widths and enumerators of everything it contains, the same on every
  compiler and configuration) differs from the reader's
  type, is refused and must be recooked. `schemaHash(type)` belongs in the derived-data cache key.
- **The reader never trusts its input.** Every count and length is checked against the bytes
  left and against the allocation budget before anything is allocated, booleans and enum values
  are validated, nesting is limited to 64 structs and arrays, and the payload must be consumed exactly. Errors carry the byte
  offset and the path of the value.
- A `std::vector` of elements that encode to zero bytes (empty structs) is refused, because its
  length couldn't be checked against the data.
- Bulk data laid out for memory mapping comes with the asset formats that need it.

## Entity fields

An `ith::Entity` is a runtime handle, so the archives never write its bits
([ADR-0032](../decisions/0032-entity-references-stable-ids-and-world-copy.md)). A read or write
that reaches a saved `Entity` field (one that isn't `Transient`) needs an
`ith::serialization::EntityMapping` (`Document.h`), or it fails with
`serialization.no_entity_mapping`:

```cpp
ith::Uuid toId(void* context, ith::Entity entity) { return static_cast<Level*>(context)->idOf(entity); }
ith::Entity toEntity(void* context, const ith::Uuid& id) { return static_cast<Level*>(context)->find(id); }

const ith::serialization::EntityMapping mapping{.toId = &toId, .toEntity = &toEntity, .context = &level};
const auto text = ith::serialization::writeText(kSquadFormat, squad, nullptr, &mapping);
const auto read = ith::serialization::readText(*text, kSquadFormat, squad, {.entities = &mapping});
```

- With a mapping, an `Entity` is written as the `ith::Uuid` `toId` gives: its canonical string in
  text, 16 bytes in binary (the schema hash counts that width). Reading calls `toEntity`.
- The null handle is always the nil Uuid, and the nil Uuid always reads as the null handle; the
  mapping isn't called for them.
- A type that holds no `Entity` needs no mapping. The level-documents format defines the
  mapping that writes entities as their stable ids.

## Errors

Every read and write returns `ith::Expected<..., ith::serialization::Error>`. An error has a
stable `code` (`serialization.wrong_type`), a `message`, the `source` passed by the caller, the
`path` of the value (`$.loadout.items[3].count`), the line and column of a text syntax error and
the byte offset of a binary error. `toString()` gives one line for logs:

```text
loadout.json: $.history[1].where.x: expected a number, found a string [serialization.wrong_type]
```

The codes each function returns are listed in its header.

## Thread safety

All functions are thread-safe as long as each call has its own target object. `Value` trees are
not thread-safe.

## Third-party code

JSON parsing and printing use [yyjson](https://github.com/ibireme/yyjson) 0.13.0, built with its
file functions and incremental reader turned off and a depth limit of 64. It is used only in
`Json.cpp` and allocates through the module's allocator table; its types never appear in a
header ([third-party.md](third-party.md)).

## Tests

- `serialization` checks the JSON reader and writer, both archives, `toValue` and `fromValue`,
  every error code with its path, versions and migrations, and the P1 exit criterion: every
  reflected type of the test modules is filled with non-default values, written and read through
  both archives and compared with its own equality, and writing the result again gives the same
  bytes. Floats are checked bit for bit over special and pseudo-random values. Golden schema hashes catch a hash that
  differs between compilers or configurations, and allocation-budget tests read small inputs
  that would otherwise allocate hundreds of MiB.
- In Clang ASan builds, `serialization_text_fuzz` and `serialization_binary_fuzz` run short
  libFuzzer sessions over the readers (`tests/serialization/*Fuzz.cpp`) with a 512 MiB malloc
  limit. They are seeded from `tests/serialization/fuzz_seeds/text` and `/binary`; the text
  target also uses `fuzz_seeds/json.dict` and runs the migration runner. These builds compile
  everything with libFuzzer's coverage instrumentation, so the fuzzer sees the engine code it
  reaches. After a change to the binary layout of the test types, regenerate the binary seeds:
  each is a 0 byte followed by `writeBinary` output of a filled test value.
