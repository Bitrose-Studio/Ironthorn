# Architecture decision records

Every architecture choice gets an ADR. Copy [0000-template.md](0000-template.md) and take the
next number. An accepted ADR is changed only by a new ADR that supersedes it.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-cpp-language-and-safety-rules.md) | C++20 as the engine language, with safety rules | Accepted |
| [0002](0002-git-workflow-and-versioning.md) | Git workflow and versioning | Accepted |
| [0003](0003-clean-room.md) | Clean-room implementation of ideas from other engines | Accepted |
| [0004](0004-modules-and-layering.md) | Engine modules and layering | Accepted |
| [0005](0005-reference-hardware.md) | Reference hardware for scalability tiers | Accepted, amended by [0022](0022-gpu-minimum-features-completed.md) |
| [0006](0006-math-and-coordinate-conventions.md) | Math library and coordinate conventions | Accepted |
| [0007](0007-logging-and-profiling.md) | Logging and profiling | Accepted |
| [0008](0008-editor-familiar-to-unreal-developers.md) | An editor familiar to Unreal Engine developers | Accepted |
| [0009](0009-rhi.md) | Rendering hardware interface over Vulkan and Direct3D 12 | Accepted, amended by [0021](0021-rhi-device-structure-and-backend-modules.md) |
| [0010](0010-serialization.md) | Serialization formats and versioning | Accepted, amended by [0020](0020-serialization-rules-completed.md) |
| [0011](0011-shader-language-and-pipeline.md) | Shader language and pipeline | Accepted, amended by [0023](0023-build-time-shaders-for-samples-and-tests.md) |
| [0012](0012-windowing-and-event-loop.md) | Windowing and the platform event loop | Accepted |
| [0013](0013-input.md) | Input: events, state, routing and consumption | Accepted |
| [0014](0014-ecs-storage-and-world.md) | ECS storage and the world | Accepted |
| [0015](0015-asset-identity-and-reimport.md) | Asset identity and non-destructive reimport | Accepted |
| [0016](0016-gameplay-model-on-the-ecs.md) | Gameplay model on the ECS | Accepted |
| [0017](0017-runtime-and-editor-separation.md) | Runtime and editor separation | Accepted |
| [0018](0018-reflection.md) | Reflection | Accepted, amended by [0019](0019-reflection-rules-corrected.md) |
| [0019](0019-reflection-rules-corrected.md) | Reflection: rules corrected after the first implementation | Accepted |
| [0020](0020-serialization-rules-completed.md) | Serialization: rules completed after the first implementation | Accepted |
| [0021](0021-rhi-device-structure-and-backend-modules.md) | RHI: device structure and backend modules | Accepted |
| [0022](0022-gpu-minimum-features-completed.md) | GPU minimum features completed for the RHI (amends 0005) | Accepted |
| [0023](0023-build-time-shaders-for-samples-and-tests.md) | Build-time shaders for samples and tests (amends 0011) | Accepted |

## Planned

These decisions are still open. Each is needed by the [roadmap](../architecture.md#roadmap) phase shown:

| Topic | Needed by |
|---|---|
| Editor UI toolkit | P2 |
| Material model (OpenPBR-based real-time subset, per-tier approximations) | P3 |
| Animation retargeting (rigs, chain mapping, runtime and editor batch retarget) | P5 |
