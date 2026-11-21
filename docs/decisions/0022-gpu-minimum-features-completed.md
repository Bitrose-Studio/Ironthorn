# ADR-0022: GPU minimum features completed for the RHI

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** amends [ADR-0005](0005-reference-hardware.md) (minimum spec);
  [ADR-0009](0009-rhi.md) (RHI, rules 8 and 17),
  [ADR-0021](0021-rhi-device-structure-and-backend-modules.md)

## Context

The Vulkan backend needs four core Vulkan 1.0 features that ADR-0005's minimum list doesn't
name, because the RHI's API already depends on them. Each is checked at start-up (ADR-0009
rule 17), so it must be part of the minimum, not an optional capability. Buffer device address
is already on the list and stays as it is.

Coverage, from [gpuinfo.org](https://vulkan.gpuinfo.org/listfeaturescore10.php) (checked
2026-11-21, share of reports with the feature):

| Feature | Windows | Linux | All platforms |
|---|---|---|---|
| `robustBufferAccess` | 100 % | 100 % | 100 % |
| `independentBlend` | 100 % | 100 % | 100 % |
| `imageCubeArray` | 100 % | 100 % | 99.92 % |
| `multiDrawIndirect` | 99.81 % | 97.94 % | 81.29 % |

The gaps are mobile and old GPUs below ADR-0005's supported generations. Every GPU that ADR-0005
supports (Turing, RDNA 1, Arc and newer, Steam Deck) reports all four. Lavapipe wasn't checked
against a single report; the functional CI run on it (ADR-0005 rule 6) is the check.

## Options considered

- **Amend ADR-0005 by this ADR** (chosen), as ADR-0019, ADR-0020 and ADR-0021 did: an accepted
  ADR is changed only by a new one.
- **Make them optional capabilities.** Each would need a fallback path at the Low tier for
  hardware the engine doesn't support anyway.
- **Leave them to the code.** The minimum spec is what store pages and the start-up check
  promise; it belongs in the ADR.

## Decision

1. **Added to the Vulkan minimum of ADR-0005 rule 1:**

   | Vulkan feature | Why the RHI needs it | Direct3D 12 equivalent |
   |---|---|---|
   | `multiDrawIndirect` | GPU-driven draws (`drawIndexedIndirectCount`) issue many argument records per call; Vulkan allows more than one indirect draw per call only with it. | `ExecuteIndirect` with a count buffer and `MaxCommandCount` above 1; always available. |
   | `independentBlend` | A graphics pipeline sets blend state per color target (up to `kMaxColorTargets`). | `D3D12_BLEND_DESC::IndependentBlendEnable`; always available. |
   | `imageCubeArray` | A cube texture may have any multiple of 6 layers, and shaders sample it as a cube array. | `TextureCubeArray` views; always available at feature level 12_0. |
   | `robustBufferAccess` | Out-of-bounds access through buffer descriptors is memory-safe, as it is on D3D12. | Defined behaviour of buffer views: out-of-bounds reads return 0 and writes are discarded; always on. |

2. **Buffer device address** stays on the minimum list unchanged. Its Direct3D 12 counterpart is
   the resource GPU virtual address, always available. Shaders still reach buffers only by
   bindless index (ADR-0009 rule 8).
3. **Robustness is memory safety, not identical results.** With `robustBufferAccess`, an
   out-of-bounds read on Vulkan may return zero or other values from the same buffer, and a write
   may be discarded or land inside the same buffer. Neither API touches other memory. Shaders must
   not depend on the value of an out-of-bounds read; Direct3D 12's zeros are not a contract.
   **Why:** the guarantee needed is that a bad bindless offset can't corrupt or crash outside its
   buffer. Zero-defined reads would need `VK_EXT_robustness2`, an extra extension for no
   behaviour the engine relies on.

## Consequences

- The start-up check names all four features; a device without one fails with
  `rhi.unsupported_device`. No supported GPU is excluded by them.
- `robustBufferAccess` can cost shader performance on some drivers. It is measured on the lab
  machines when the renderer draws its first scenes.
- `docs/development/rhi.md` lists the full Vulkan minimum, including these four.
- ADR-0005 is marked as amended by this ADR in its header and in the index.
- **Revisit when:** profiles show `robustBufferAccess` costs frame time on a reference machine
  (consider enabling it only in Debug and development builds), or a shader needs defined
  out-of-bounds values (consider `VK_EXT_robustness2`).
