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
| [0009](0009-rhi.md) | Rendering hardware interface over Vulkan and Direct3D 12 | Accepted |
| [0010](0010-serialization.md) | Serialization formats and versioning | Accepted |
| [0011](0011-shader-language-and-pipeline.md) | Shader language and pipeline | Accepted |
| [0012](0012-windowing-and-event-loop.md) | Windowing and the platform event loop | Accepted |
| [0013](0013-input.md) | Input: events, state, routing and consumption | Accepted |
| [0014](0014-ecs-storage-and-world.md) | ECS storage and the world | Accepted |
| [0015](0015-asset-identity-and-reimport.md) | Asset identity and non-destructive reimport | Accepted |
| [0016](0016-gameplay-model-on-the-ecs.md) | Gameplay model on the ECS | Accepted |

## Planned

These decisions are still open. Each is needed by the [roadmap](../architecture.md#roadmap) phase shown:

| Topic | Needed by |
|---|---|
| Runtime and editor separation | P1 |
| Editor UI toolkit | P2 |
| Animation retargeting (rigs, chain mapping, runtime and editor batch retarget) | P5 |
