# Coding standards

The language rules are in [ADR-0001](../decisions/0001-cpp-language-and-safety-rules.md). This
page covers design, style and day-to-day practice. Formatting and naming are checked by tools:
`scripts/dev.ps1 format` (clang-format) and `scripts/dev.ps1 tidy` (clang-tidy).

**Decision rule:** project standard first, consistency second, personal preference last.

## Design: simple first, SOLID where it pays

**The simplest solution that gives the same result wins (KISS).** Don't build for needs that
don't exist yet:

- no speculative abstractions;
- no frameworks inside the engine;
- no configuration nobody asked for;
- no interface with a single implementation unless a test or a module boundary needs it.

When code does need structure, it follows the SOLID principles:

| Principle | In Ironthorn |
|---|---|
| **Single responsibility** | A module, class or function has one reason to change. Split code when two concerns change for different reasons, not to make files smaller. |
| **Open/closed** | Extend behaviour by adding code (a new module, component, system or backend), not by editing switch statements across the engine. |
| **Liskov substitution** | Every implementation of an interface (e.g. each RHI backend) honours the whole contract, and the same tests run against all of them. |
| **Interface segregation** | Small, focused interfaces. A caller depends only on what it uses. |
| **Dependency inversion** | Higher layers depend on interfaces owned by lower layers, and third-party libraries hide behind engine interfaces ([ADR-0004](../decisions/0004-modules-and-layering.md)). |

If SOLID and KISS pull in different directions, choose the simpler code. Add structure when a
second real use appears.

## Naming (checked by clang-tidy)

| Kind | Style | Example |
|---|---|---|
| Namespaces | `lower_case` | `ith`, `ith::build_info` |
| Types, enums, enum values, type aliases | `CamelCase` | `AssertInfo`, `AssertAction::Abort` |
| Functions, methods, parameters, variables | `camelBack` | `setAssertHandler`, `frameIndex` |
| Private and protected members | `m_` + `camelBack` | `m_storage` |
| Globals | `g_` + `camelBack` (avoid them) | `g_assertHandler` |
| Constants and `constexpr` variables | `k` + `CamelCase` | `kEngineVersion` |
| Macros | `ITH_` + `UPPER_CASE` | `ITH_ASSERT` |
| Files | `CamelCase.h` / `.cpp`, one main type per file | `Expected.h` |

## Layout

- 4 spaces, 120 columns, braces always (clang-format inserts them).
- Include order, sorted by clang-format:
  1. the file's own header;
  2. `"ith/..."` engine headers;
  3. other project headers;
  4. third-party `<...>` headers;
  5. standard library headers.
- Headers start with `#pragma once` and a short comment that says what the file is for.

## Documentation

- **Every public declaration states its contract:**
  - what it does;
  - its preconditions (which are asserted);
  - how it fails;
  - who owns what;
  - whether it is thread-safe.
- **Comments say why, not what.** Code that needs a comment to explain *what* it does should
  be simplified first.
- **Every module has a short section in [architecture.md](../architecture.md)** or a page under
  `docs/`. It says what the module is for and what it must not do.
- **Decisions go into ADRs**, not into code comments or commit messages alone.

## Practice

- **Errors.**
  - Use `ith::Expected` for failures the program must handle.
  - Use `ITH_ASSERT` for programmer errors.
  - Use `ITH_FATAL` only where continuing would corrupt data.
  - Error codes are stable dotted strings (`io.not_found`). Messages say what went wrong and,
    where possible, what to do about it.
- **No hidden global state.** Engine state is owned by objects that are passed explicitly. The
  few process-wide hooks (like the assert handler) are thread-safe and documented.
- **Tests come with the change:**
  - normal cases, edge cases and failure paths (asserts are tested with
    `ith::test::AssertCapture`);
  - a regression test for every bug fix;
  - tests check behaviour, not implementation details.
- **Dependencies are deliberate.**
  - Each one is pinned in `cmake/IronthornDependencies.cmake`.
  - Each one is listed in [third-party.md](third-party.md) with its license.
  - Its types never appear in public module headers.
- **Platform code stays in the platform layer.** No `#ifdef _WIN32` elsewhere unless a comment
  explains why.
- **Clean room.** No code from other engines or their plugins ([ADR-0003](../decisions/0003-clean-room.md)).
