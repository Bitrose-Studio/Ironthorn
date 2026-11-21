# ADR-0020: Serialization: rules completed after the first implementation

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** amends [ADR-0010](0010-serialization.md) (serialization);
  [ADR-0015](0015-asset-identity-and-reimport.md) (derived-data cache key),
  [ADR-0017](0017-runtime-and-editor-separation.md) (`EditorOnly`),
  [ADR-0018](0018-reflection.md) and [ADR-0019](0019-reflection-rules-corrected.md) (reflection)

## Context

The first serialization slice had to decide details ADR-0010 left open, and one rule (error
positions) couldn't be kept as written. This ADR records those rules. Everything else in
ADR-0010 stands.

## Options considered

- **Amend ADR-0010 by this ADR** (chosen), as ADR-0019 did for reflection: the accepted record
  stays readable and each addition carries its reason.
- **Supersede ADR-0010 with a full rewrite.** Repeats the unchanged rules for nine additions.
- **Leave the details to the code.** Formats are long-lived contracts with tools and files on
  disk; their rules belong in an ADR.

## Decision

1. **Text document layout:** `{"format": <name>, "version": <n>, "data": <value>}`. Any
   reflected type, or a value tree, can be the root. Migrations operate on the `data` subtree,
   and paths start at `$`, which is `data`. **Why:** one envelope for every format, and
   migrations can't touch the header by accident.
2. **Error positions** (changes ADR-0010 rule 2): syntax errors carry line, column and byte
   offset; value errors (wrong type, out of range, unknown enumerator) carry a JSON path such as
   `$.items[3].count` instead. **Why:** the value tree doesn't keep source positions, and keeping
   them would cost memory on every load for errors that are rare.
3. **Binary header:** magic `ITHB`, u32 format version, u64 **schema hash**, u64 payload size,
   u16 name length and the name, then zero padding so the payload starts at a 16-byte offset.
   - The schema hash covers the encoded layout of every type the root reaches: type ids, kinds,
     encoded widths (not C++ object sizes), saved field ids, enumerator names and values. It is
     the same on every compiler, platform and build configuration. It is computed with
     `ith::fnv1a64`, the same function as type ids (ADR-0018 rule 7).
   - A mismatch is refused, which means recook. The schema hash is part of the derived-data
     cache key, next to the output format version of ADR-0015.
   - **Why:** cooked data doesn't migrate (ADR-0010 rule 14), so a reflected type that changed
     without a version bump must be caught, not misread.
4. **Floats:** text uses the shortest round-trip form from `std::to_chars`; a non-finite value
   is a text write error. Binary keeps every value bit-exact, non-finite included. **Why:** JSON
   has no NaN or infinity, while cooked data must round-trip exactly.
5. **Flags** (ADR-0018):
   - `EditorOnly` fields are saved in text (source data) and dropped in binary (cooked). A root
     or container element of an `EditorOnly` or `Transient` type is `not_saved` in binary.
   - `Transient` fields are never saved; a `Transient` field found in a document is treated as
     unknown (ADR-0010 rule 13).
   - `SaveGame` has no effect until the save-system ADR.
   - **Why:** source files keep what the editor needs; cooked data keeps what the game needs
     (ADR-0017 rule 9).
6. **Edge rules:**
   - `std::optional<std::optional<T>>` is refused in text, where "empty outer" and "empty
     inner" would both be `null`.
   - Vectors of zero-byte elements are refused in binary, where a count without bytes would
     defeat the size checks of ADR-0010 rule 8.
   - Binary strings are not validated as UTF-8; their consumers accept arbitrary bytes. Text is
     UTF-8 by JSON's rules.
   - **Depth** (limit 64 in both): text counts every object and array, the document object
     included; binary counts structs and arrays.
7. **Migrations:** a format declares a `minimumVersion` and exactly `version - minimumVersion`
   migration functions (N to N + 1) on the value tree, run all-or-nothing. A format without
   migrations sets `minimumVersion` to `version`. Retiring migrations (ADR-0010 rule 11) raises
   `minimumVersion`. **Why:** a missing or extra step is caught when the format is registered,
   not when an old file is loaded.
8. **Allocation budget on reads.** Both readers charge every allocation against a per-read
   budget: container elements by element size, strings by bytes, and the text value tree by
   node count × node size. The default is max(64 MiB, 16 × input size); the caller can override
   it, and can also pass a smaller maximum document size (for example for config files).
   Exceeding the budget fails with `serialization.too_large` before allocating. **Why:** without
   exceptions an allocation failure ends the process, and the size check against the remaining
   bytes alone lets a small input request far more memory than its own size (a vector of large
   structs whose saved part is small). ADR-0010's "safe on untrusted input" needs a budget.
9. **Deferred:**
   - yyjson's allocator table forwards to the C heap until the engine has a general allocator;
     it is one hook to change then (ADR-0010 rule 6).
   - Alignment of bulk sections for memory mapping (ADR-0010 rule 7) comes with the asset
     formats that need it.

## Consequences

- Errors point to a line for broken syntax and to a path for wrong values; tools and the editor
  show both.
- Any change to a reflected type reached by cooked data changes its schema hash and forces a
  recook of exactly that data.
- Format registration fails fast on a broken migration chain.
- Untrusted input can't make a read allocate more than its budget; a caller loading unusually
  large data raises the budget explicitly.
- Follow-up work: tests for each rule above (envelope, paths, header and hash stability across
  the CI compilers, flags, edge rules, migration count, allocation budget); the allocator hook;
  aligned bulk sections with the asset formats.
- **Revisit when:** keeping source positions for value errors proves worth its memory, or the
  save-system ADR gives `SaveGame` a meaning.
