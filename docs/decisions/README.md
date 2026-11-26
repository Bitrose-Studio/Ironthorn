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
| [0007](0007-logging-and-profiling.md) | Logging and profiling | Accepted, amended by [0044](0044-gpu-profiling-zones.md) |
| [0008](0008-editor-familiar-to-unreal-developers.md) | An editor familiar to Unreal Engine developers | Accepted, amended by [0037](0037-editor-commands-transactions-selection-and-automation.md) |
| [0009](0009-rhi.md) | Rendering hardware interface over Vulkan and Direct3D 12 | Accepted, amended by [0021](0021-rhi-device-structure-and-backend-modules.md), [0044](0044-gpu-profiling-zones.md) |
| [0010](0010-serialization.md) | Serialization formats and versioning | Accepted, amended by [0020](0020-serialization-rules-completed.md) |
| [0011](0011-shader-language-and-pipeline.md) | Shader language and pipeline | Accepted, amended by [0023](0023-build-time-shaders-for-samples-and-tests.md) |
| [0012](0012-windowing-and-event-loop.md) | Windowing and the platform event loop | Accepted, amended by [0025](0025-job-system.md), [0027](0027-input-rules-completed.md) |
| [0013](0013-input.md) | Input: events, state, routing and consumption | Accepted, amended by [0027](0027-input-rules-completed.md) |
| [0014](0014-ecs-storage-and-world.md) | ECS storage and the world | Accepted, amended by [0030](0030-ecs-hooks-registration-and-immutable-components.md), [0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md), [0032](0032-entity-references-stable-ids-and-world-copy.md) |
| [0015](0015-asset-identity-and-reimport.md) | Asset identity and non-destructive reimport | Accepted, amended by [0024](0024-asset-document-envelope-and-extension.md), [0033](0033-reflection-property-paths-and-patches.md), [0034](0034-level-documents.md) |
| [0016](0016-gameplay-model-on-the-ecs.md) | Gameplay model on the ECS | Accepted |
| [0017](0017-runtime-and-editor-separation.md) | Runtime and editor separation | Accepted |
| [0018](0018-reflection.md) | Reflection | Accepted, amended by [0019](0019-reflection-rules-corrected.md), [0030](0030-ecs-hooks-registration-and-immutable-components.md), [0032](0032-entity-references-stable-ids-and-world-copy.md), [0033](0033-reflection-property-paths-and-patches.md) |
| [0019](0019-reflection-rules-corrected.md) | Reflection: rules corrected after the first implementation | Accepted |
| [0020](0020-serialization-rules-completed.md) | Serialization: rules completed after the first implementation | Accepted, amended by [0024](0024-asset-document-envelope-and-extension.md), [0032](0032-entity-references-stable-ids-and-world-copy.md), [0033](0033-reflection-property-paths-and-patches.md) |
| [0021](0021-rhi-device-structure-and-backend-modules.md) | RHI: device structure and backend modules | Accepted |
| [0022](0022-gpu-minimum-features-completed.md) | GPU minimum features completed for the RHI (amends 0005) | Accepted |
| [0023](0023-build-time-shaders-for-samples-and-tests.md) | Build-time shaders for samples and tests (amends 0011) | Accepted |
| [0024](0024-asset-document-envelope-and-extension.md) | Asset documents: the envelope id and native asset extensions (amends 0015, 0020) | Accepted, amended by [0034](0034-level-documents.md) |
| [0025](0025-job-system.md) | The job system (amends 0012) | Accepted, amended by [0026](0026-job-system-rules-corrected.md), [0035](0035-file-system-configuration-and-main-thread-queue.md) |
| [0026](0026-job-system-rules-corrected.md) | Job system: rules corrected after the first implementation (amends 0025) | Accepted |
| [0027](0027-input-rules-completed.md) | Input: rules completed after the first implementation (amends 0012, 0013) | Accepted, amended by [0035](0035-file-system-configuration-and-main-thread-queue.md) |
| [0028](0028-render-graph-v1.md) | Render graph v1 | Accepted |
| [0029](0029-editor-ui-toolkit.md) | Editor UI toolkit | Accepted, amended by [0035](0035-file-system-configuration-and-main-thread-queue.md), [0040](0040-editor-docking-panels-and-layouts.md), [0042](0042-editor-viewport.md) |
| [0030](0030-ecs-hooks-registration-and-immutable-components.md) | ECS: component hooks registration and immutable components (amends 0014, 0018) | Accepted |
| [0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md) | ECS: hierarchy, enabled state and transform propagation completed (amends 0014) | Accepted, amended by [0034](0034-level-documents.md) |
| [0032](0032-entity-references-stable-ids-and-world-copy.md) | Entity references, stable ids and world copy (amends 0014, 0018, 0020) | Accepted, amended by [0034](0034-level-documents.md), [0037](0037-editor-commands-transactions-selection-and-automation.md) |
| [0033](0033-reflection-property-paths-and-patches.md) | Reflection property paths and patches (amends 0015, 0018, 0020) | Accepted, amended by [0037](0037-editor-commands-transactions-selection-and-automation.md) |
| [0034](0034-level-documents.md) | Level documents (amends 0015, 0024, 0031, 0032) | Accepted |
| [0035](0035-file-system-configuration-and-main-thread-queue.md) | File system, layered configuration and the main-thread queue (amends 0025, 0027, 0029) | Accepted, amended by [0036](0036-file-system-windows-atomic-replace-corrected.md), [0041](0041-editor-amendments-after-outliner-and-details.md) |
| [0036](0036-file-system-windows-atomic-replace-corrected.md) | File system: Windows atomic replace corrected after the first implementation (amends 0035) | Accepted, amended by [0038](0038-file-system-windows-read-retry-during-replace.md) |
| [0037](0037-editor-commands-transactions-selection-and-automation.md) | Editor commands, transactions, selection and automation (amends 0008, 0032, 0033) | Accepted, amended by [0041](0041-editor-amendments-after-outliner-and-details.md), [0042](0042-editor-viewport.md) |
| [0038](0038-file-system-windows-read-retry-during-replace.md) | File system: Windows read retry while a replace is in flight (amends 0036) | Accepted, amended by [0039](0039-file-system-windows-read-retry-measured.md) |
| [0039](0039-file-system-windows-read-retry-measured.md) | File system: Windows read retry after a replace, measured (amends 0038) | Accepted |
| [0040](0040-editor-docking-panels-and-layouts.md) | Editor docking, panels and layouts (amends 0029) | Accepted |
| [0041](0041-editor-amendments-after-outliner-and-details.md) | Editor amendments after the Outliner and Details (amends 0035, 0037) | Accepted |
| [0042](0042-editor-viewport.md) | Editor world and viewport (amends 0029, 0037) | Accepted, amended by [0043](0043-editor-ui-user-texture-state.md) |
| [0043](0043-editor-ui-user-texture-state.md) | Editor UI user textures: the renderer owns the import state (amends 0042) | Accepted |
| [0044](0044-gpu-profiling-zones.md) | GPU profiling zones (amends 0007, 0009) | Accepted |

## Planned

These decisions are still open. Each is needed by the [roadmap](../architecture.md#roadmap) phase shown:

| Topic | Needed by |
|---|---|
| Asset streaming (async, budgeted loading of textures, geometry and audio on the jobs background lane, per-tier budgets, requests ordered by distance, screen size and visibility) | P3, before texture work |
| Runtime settings and scalability tiers (named settings with their registered cost, read when they change and never per frame; tier presets as config layers; console access in development builds) | P3, before the first renderer feature |
| Pipeline state caching (prebuilt pipeline caches, warm-up before gameplay, no shader or pipeline compile hitches in shipped games) | P3 |
| Performance regression captures (scripted camera paths recording frame time, CPU and GPU zones and memory per tier, compared against budgets in CI) | P3, with the first renderer |
| Material model (OpenPBR-based real-time subset, per-tier approximations) | P3 |
| Animation retargeting (rigs, chain mapping, runtime and editor batch retarget) | P5 |
| Plugin and project manifests (plugin name, version, dependencies, provided services, settings schema, editor counterpart; how a project enables plugins; only enabled plugins link into a shipped game) | P5, before the abilities plugin |
| Destruction plugin (pre-fractured meshes authored at import, fracture on impact through physics, debris budgets and cleanup per tier) | After P8, once physics and the renderer are stable |
