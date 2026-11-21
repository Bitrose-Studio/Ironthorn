# ADR-0019: Reflection: rules corrected after the first implementation

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** amends [ADR-0018](0018-reflection.md) (reflection);
  [ADR-0016](0016-gameplay-model-on-the-ecs.md) (hot reload boundary),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds)

## Context

Reviews of the first reflection slice found ADR-0018 rules that the implementation can't keep
as written, or that leave gaps. This ADR changes only those rules. Everything else in ADR-0018
stands.

## Options considered

- **Amend ADR-0018 by this ADR** (chosen). The accepted record stays readable and each change
  carries its reason.
- **Supersede ADR-0018 with a full rewrite.** Repeats the unchanged rules for seven
  corrections.
- **Keep the rules and change the code around them.** Rule 6 can't be kept without forbidding
  strings, arrays and optionals in MSVC Debug builds, which the ADR allows.

## Decision

1. **Rule 6 is replaced: no bases and no virtual functions.** A reflected type has no base
   classes (the scanner refuses them) and isn't polymorphic (a generated
   `static_assert(!std::is_polymorphic_v<T>)`).
   - **Why:** in MSVC Debug builds `std::string`, `std::vector` and `std::optional` are not
     standard-layout, so the old rule forbade field types that rule 5 allows.
   - `offsetof` on such types is conditionally supported; GCC, Clang and MSVC support it and
     refuse it only for members reached through virtual bases, which this rule excludes. The
     generated files silence `-Winvalid-offsetof`.
   - `[[no_unique_address]]`, `[[msvc::no_unique_address]]` and `alignas` on reflected fields
     are refused: a potentially overlapping member breaks destroying and constructing a field in
     place.
2. **Rule 4's syntax is made precise.**
   - `ITH_TYPE(...)` expands to a friend declaration, a static `ithTypeInfo()` declaration and a
     declaration-only marker member. A type derived from a reflected type is therefore not
     treated as reflected: using it as one is a compile error.
   - `ITH_ENUM(Name, attributes...)` is written **after** the enum, without a semicolon, and
     declares `ithEnumTypeInfo(Name*)` in the enum's namespace, found by argument-dependent
     lookup. The scanner refuses a name that doesn't match the enum directly before it.
   - Using an unannotated enum as a reflected field type is a compile error. Enums with a
     `bool` underlying type are refused.
   - **Why:** the original example put the macro between `enum class` and the name, which
     isn't valid C++ for a macro expanding to declarations; the marker member keeps
     derivation from silently inheriting reflection.
3. **Rule 2's output path is per configuration:**
   `generated/<module>/<CONFIG>/<module>.reflect.cpp`. **Why:** each configuration builds its
   own generator, so their outputs must not overwrite each other. The generator links the
   `foundation` module, so it hashes names with the same `ith::fnv1a64` the runtime uses; it
   still uses no third-party code.
4. **Rule 11: Shipping also keeps `Unit` and `FormerName`.** **Why:** `FormerName` is needed
   at run time to load old save data; `Unit` is small and gives values their meaning.
5. **`FormerName` collisions inside one type are a generation error:** a field's `FormerName`
   equal to another field's name, or two fields with the same `FormerName`. **Why:** loading
   would be ambiguous.
6. **Records are compared by id, never by address.** Records of container types are generated
   per binary, so two records of one type can exist in one process. Registry entries of a
   module are removed before it unloads; the hot reload ADR defines when.
7. **Until ADR-0017's `KIND` exists, a module's kind is derived from its layer**: `editor`
   layer modules count as `editor`, all others as `runtime`. This is temporary and ends when
   `KIND` lands.

## Consequences

- Strings, arrays and optionals stay usable as fields on every compiler and configuration.
- Composition instead of inheritance, already ADR-0018's rule, is now enforced by the scanner
  and the compiler.
- Shipping records grow slightly by units and former names.
- Code comparing type records by pointer is a bug; reviews and tests check for it.
- Follow-up work: tests for each refused construct above; drop rule 7 when `KIND` lands.
- **Revisit when:** a compiler refuses `offsetof` on a reflected type, or a real type needs a
  base class.
