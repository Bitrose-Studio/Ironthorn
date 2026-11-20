# ADR-0010: Serialization formats and versioning

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, generated
  reflection, fuzz targets), [ADR-0004](0004-modules-and-layering.md) (serialization in `core`,
  no third-party types in public headers), [architecture](../architecture.md)

## Context

Assets, scenes, prefabs, config and editor commands are saved and loaded through one
serialization layer, driven by generated reflection. Two kinds of data:

- **Source data** (assets, scenes, config) is written by the editor and kept in git. It must
  diff cleanly, survive engine upgrades and be readable by people.
- **Cooked data** is produced by the cooker and read by the game. It must load fast, possibly
  from memory-mapped files, and is rebuilt from source whenever needed.

Requirements: no exceptions and no RTTI (ADR-0001); errors as `ith::Expected`; read **and**
write with stable key order; exact round-trip of integers and floats; safe on untrusted input
(mods, downloaded content), so depth-limited and fuzzed; no library types in public headers;
C++20 only; as few libraries as possible.

Lessons from earlier engine prototypes by the same maintainer:

- A JSON library type that was the public serialization interface spread into 37 headers, and
  the wrapper caught the library's exceptions. Replacing it meant rewriting everything above
  math. The library must stay behind engine types.
- Formats without a version and migrations broke existing files on every schema change. A
  header with a format name and version, plus step-wise, all-or-nothing migrations, worked.
- A binary reader with a sticky failure state that checks counts against the remaining bytes
  before allocating held up well on untrusted input.

Facts below were checked on 2026-11-20 against release tags and sources, and the chosen library
was compiled with Clang 19.1.5 as C++20 with `-fno-exceptions -fno-rtti -Werror`.

## Options considered

### Text format and library

JSON is the text format: universally tooled, diffable, and enough for source assets. TOML or
JSON5 would add a second format for no gain.

- **yyjson 0.13.0** (MIT, [release](https://github.com/ibireme/yyjson/releases/tag/0.13.0),
  2026-09-08). Plain C, one `.c` and one `.h`, so no exceptions by construction. Reads and
  writes, with a mutable tree, key order preserved, pretty output, a custom allocator
  (`yyjson_alc`), strict RFC 8259 parsing with UTF-8 validation, optional comments, and OSS-Fuzz
  coverage. Caveats: pre-1.0 API (0.13.0 changed a setter's signature), one main author, objects
  are linked lists (slow random key lookup; reading in reflection order avoids it).
- **nlohmann/json 3.12.0** (MIT). Throws by design; `JSON_NOEXCEPTION` turns every error into
  an abort, which makes a malformed file fatal.
- **glaze 9.1.0** (MIT). The best API, but requires C++23 and `/Zc:preprocessor`.
- **simdjson 5.2.0** (Apache-2.0 or MIT). The fastest parser, but no mutable tree, padded input,
  and a builder path that throws unless the error-code pattern is used. Speed doesn't matter at
  source-asset sizes; cooked data is binary.
- **RapidJSON** (MIT). Last release v1.1.0 in 2016; only commit pins are possible.
- **Our own JSON reader and writer.** Feasible with `std::from_chars`/`std::to_chars`, but it
  needs its own UTF-8 validation, fuzzing and maintenance, for the same result as yyjson.

### Binary cooked data

- **Our own binary archive**, driven by the same reflection as the text archive. One schema (the
  C++ types) serves both, and bulk data (meshes, textures) can be laid out for memory mapping and
  in-place loading.
- **FlatBuffers v25.12.19** (Apache-2.0). Zero-copy, but a second schema language (`.fbs`) and a
  `flatc` code generator next to our reflection.
- **bitsery v5.2.5** (MIT, header-only, no exceptions). A good typed reader/writer, but little
  activity (last commit 2025-10-08) and nothing a reflection-driven archive lacks.

## Decision

### Module and API

1. **`core/serialization`** owns all serialization. Its public API is engine types only: a
   reader and writer for each format, driven by reflection, and a small value tree for free-form
   data (editor commands, config). **Only this module's source files include yyjson.**
2. Every read returns `ith::Expected<T, serialization::Error>`; the error names the file, the
   position (line and column for text, offset for binary) and the field path.

### Text: JSON through yyjson

3. **Source data is JSON** (RFC 8259, UTF-8, LF line endings), read and written through
   **yyjson 0.13.0**, pinned by tag and SHA-256 with `YYJSON_INSTALL OFF`, file I/O disabled
   (the engine's file system does I/O) and the incremental reader disabled.
4. **Output is stable for diffs:** two-space pretty printing, a newline at the end, keys in
   reflection declaration order, integers exact (64-bit signed and unsigned). **Floats are
   formatted by the engine with `std::to_chars`** (shortest round-trip form) and handed to yyjson
   as raw numbers; yyjson's own float writer is not relied on. Non-finite floats are an error. A
   round-trip test covers every reflected type and checks that floats read back bit-identical.
5. **Assets and scenes are strict JSON.** Comments are allowed only in config files.
6. **Limits:** nesting depth at most 64 and documents at most 256 MiB, enforced by the engine
   reader. yyjson allocates through the engine allocator.

### Binary: our own archive

7. **Cooked data uses our own binary archive.** Little-endian, fixed-width types, no padding
   from the C++ layout. A blob starts with a header: magic, format name, format version and
   payload size. Bulk sections are aligned so they can be memory-mapped and used in place.
8. **The reader is hardened:** a sticky failure state; every count and size checked against the
   remaining bytes before allocating; booleans and enums validated, never read raw.

### Versioning and migration

9. **Every format has a name and a version.** A text document starts with `"format"` and
   `"version"` fields; a binary blob carries them in its header. Writers always write the current
   version.
10. **Source data migrates.** Each format keeps a chain of migrations, version N to N + 1, that
    run on the value tree before reflection reads it. A chain runs all-or-nothing: either the
    document reaches the current version or the load fails and the file is untouched. Files are
    rewritten in the new version only when they are saved.
11. **Migrations are retired at major versions.** The editor has a "resave all" command that
    loads and saves every source file of a project in the current version. Before a major engine
    release, projects run it, and the release removes migrations older than its minimum
    supported version. A file older than that is refused with an error naming that minimum.
12. **When the version changes:** adding a field with a default does not need a new version (a
    missing field takes its default). Removing, renaming or changing the meaning or type of a
    field bumps the version and adds a migration.
13. **Unknown fields.** In the editor, an unknown field is a warning naming the file and field;
    the rest loads, and the field is dropped on the next save. During cooking, an unknown field is
    an error, so stale data never reaches a game.
14. **Cooked data does not migrate.** Its format version is part of the derived-data cache key;
    a version change means a recook. The game refuses a blob whose version it doesn't know, with
    an error, not a crash.
15. **Newer than known fails.** A file whose version is newer than the engine's is refused with
    an error naming both versions; there is no forward compatibility.
16. **Fuzz targets** cover the JSON reader, the binary reader and the migration runner
    (ADR-0001).

## Consequences

- One new dependency (yyjson, MIT, small C), hidden in one module. nlohmann/json is not used.
- Source files diff cleanly and survive schema changes through migrations; cooked data stays
  simple because it is always rebuilt.
- Every breaking schema change costs a migration and a test. Migrations accumulate until the
  next major version, when "resave all" lets them be removed.
- yyjson is pre-1.0: each upgrade needs an API check. Its small size means we could maintain it
  ourselves if it stalled.
- Follow-up work: the module with value tree, text and binary archives, header and migration
  runner, round-trip and fuzz tests; the editor's "resave all" command; generated reflection;
  the asset identity ADR builds its `.meta` files on this format.
- **Revisit when:**
  - yyjson becomes unmaintained or changes its API in a way we can't absorb (fall back to our
    own reader and writer behind the same API);
  - profiles show JSON parsing on a load path that matters;
  - the save system needs a format; its ADR decides whether save files follow the source rule
    (migrate) on the binary archive.
