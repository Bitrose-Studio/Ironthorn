# Architecture decision records

Every architecture choice gets an ADR. Copy [0000-template.md](0000-template.md) and take the
next number. An accepted ADR is changed only by a new ADR that supersedes it.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-cpp-language-and-safety-rules.md) | C++20 as the engine language, with safety rules | Accepted |
| [0002](0002-git-workflow-and-versioning.md) | Git workflow and versioning | Accepted |
| [0003](0003-clean-room.md) | Clean-room implementation of ideas from other engines | Accepted |
| [0004](0004-modules-and-layering.md) | Engine modules and layering | Accepted |
| [0005](0005-reference-hardware.md) | Reference hardware for scalability tiers | Accepted |
| [0006](0006-math-and-coordinate-conventions.md) | Math library and coordinate conventions | Accepted |
| [0007](0007-logging-and-profiling.md) | Logging and profiling | Accepted |
| [0008](0008-editor-familiar-to-unreal-developers.md) | An editor familiar to Unreal Engine developers | Accepted |

## Planned

These decisions are still open. Each is needed by the [roadmap](../architecture.md#roadmap) phase shown:

| Topic | Needed by |
|---|---|
| RHI: resources, binding model, command API, frame model; Vulkan and Direct3D 12 backends | P1 |
| Shader language and pipeline (Slang, ahead-of-time compiled shaders) | P1 |
| Windowing and the platform event loop | P1 |
| Input: events, state, routing and consumption | P1 |
| ECS storage and the world | P1 |
| Gameplay model on the ECS | P1 |
| Asset identity and non-destructive reimport | P1 |
| Runtime and editor separation | P1 |
| Editor UI toolkit | P2 |
