# ADR-0004: Engine modules and layering

- **Status:** Accepted
- **Date:** 2026-11-20

## Context

An engine whose parts are tangled ends up shipping every subsystem in every game and leaking
third-party types through its interfaces. Both problems are expensive to fix later. Ironthorn
has to be modular from the first module, so a game links only what it uses.

## Decision

1. An **engine module** is a static library in `engine/ith/<name>/`, declared with
   `ith_add_module(<name> LAYER <layer> SOURCES … DEPENDS …)`. Its headers are included as
   `"ith/<name>/…"`.
2. **Layers**, lowest first ([architecture](../architecture.md#layers)):

   | Layer | Holds |
   |---|---|
   | `foundation` | memory, containers, math, strings, asserts, Expected, logging, profiling hooks |
   | `platform` | window, events, file system, threads, jobs, RHI and its backends |
   | `core` | ECS world, scheduler, reflection, serialization, assets, input routing, events |
   | `engine` | renderer, UI, physics, animation, audio, net, navigation, VFX, terrain, … |
   | `framework` | gameplay framework, abilities, AI, save, gameplay kit |
   | `editor` | editor modules |
   | `application` | editor, player, server, tools |

3. **Dependencies never point upwards.**
   - A module depends only on modules of the same or a lower layer.
   - Dependencies have no cycles.
   - A module includes only headers of modules in its dependency closure.
   - The `module_layering` test (`cmake/CheckModuleLayering.cmake`) enforces all three.
   - Same-layer dependencies are declared deliberately and kept few.
4. **The runtime never depends on the editor.** Editor-only data lives in `editor` modules.
5. **No third-party types in public module headers**, unless an ADR allows it.
6. **No empty modules.** A module is created when it has a defined responsibility and real code.
7. **Optional subsystems are opt-in** per project, through a module manifest. Its format is
   decided by a later ADR, when the first optional module lands.

## Consequences

- Layer violations fail CI instead of growing silently.
- A cycle between modules fails the test, so shared code moves into a module both can depend on.
