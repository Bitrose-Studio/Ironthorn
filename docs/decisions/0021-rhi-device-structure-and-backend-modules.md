# ADR-0021: RHI: device structure and backend modules

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** amends [ADR-0009](0009-rhi.md) (RHI);
  [ADR-0004](0004-modules-and-layering.md) (layering),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds, no static initializers)

## Context

Building the first RHI slice showed that ADR-0009 rule 2 ("abstract classes with virtual
functions") would make every backend repeat the same backend-neutral work, and that ADR-0009
doesn't say how backends are created without the `rhi` module depending on them. This ADR
settles both. Everything else in ADR-0009 stands.

## Options considered

- **Amend ADR-0009 by this ADR** (chosen), as ADR-0019 and ADR-0020 did: the accepted record
  stays readable and each change carries its reason.
- **Keep abstract `Device` and `CommandList` classes.** Each backend reimplements handle tables,
  deferred destruction, frame pacing and recording checks; the same bugs appear twice and are
  tested twice.
- **Backend selection in each application, or a registry filled by static initializers.**
  Duplicated selection logic, or registration that static libraries silently drop
  (ADR-0017 rule 5).

## Decision

1. **`rhi::Device` and `rhi::CommandList` are concrete classes** over internal backend
   interfaces, `detail::DeviceBackend` and `detail::CommandListBackend`, with one virtual call
   per RHI call. This replaces "abstract classes" in ADR-0009 rule 2. The backend-neutral work
   lives in `rhi` once: handle tables with generations, descriptor allocation, deferred
   destruction, frame pacing, swapchain image handles, device-loss bookkeeping and recording
   checks. **Why:** written once, and tested once on the null backend (rule 3) for every
   backend. The dispatch cost is unchanged from ADR-0009.
2. **Backends are separate modules:** `rhi_d3d12` (Windows only) and `rhi_vulkan`, both in the
   `platform` layer with kind `client`. Each keeps its vendor libraries private and exposes one
   factory, `createD3D12Device` or `createVulkanDevice`, like `createNullDevice`.
   - `rhi` itself has no vendor dependency and no `Device::create`: it can't call the backends
     without a dependency cycle.
   - **Backend selection** (Direct3D 12 by default on Windows, otherwise Vulkan, and a
     `backend_unavailable` error when the chosen one isn't built or can't start) lives in one
     small `platform`-layer `client` module that depends on `rhi` and the backends. It is
     created when the first backend lands. There is no selection code in applications, no
     registry and no static initializer (ADR-0017).
   - Console backends become further modules of the same shape.
   - **Why:** the `module_layering` test can check it: the renderer and the editor depend on
     `rhi` only, never on a backend.
3. **A null backend** (`createNullDevice`) ships in `rhi`, for tests and for running the editor
   or player without a GPU. It simulates the GPU timeline and injects faults: device loss,
   failed creation and an outdated swapchain. **Why:** the shared code of rule 1 and the
   device-loss path are tested on every CI machine, GPU or not.
4. **Mesh-shader commands are added with the renderer's mesh path**, not before. The
   capability flag is reported now (ADR-0009 rule 17). **Why:** no API without a user and a
   test.

## Consequences

- Backend modules shrink to translating calls into the API; shared behaviour has one
  implementation and one test suite.
- Headless CI runs the RHI's logic, including device loss, without a GPU.
- One extra module (backend selection) appears with the first real backend.
- Follow-up work: the null backend's fault injection tests; the selection module with the first
  backend; mesh-shader commands with the renderer.
- **Revisit when:** a backend needs behaviour the shared layer can't express without
  backend-specific branches, or profiles show the backend call layer costs frame time.
