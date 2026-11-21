# ADR-0005: Reference hardware for scalability tiers

- **Status:** Accepted
- **Amended by:** [ADR-0022](0022-gpu-minimum-features-completed.md)
- **Date:** 2026-11-20
- **Related:** [architecture: Scalability](../architecture.md#scalability), ADR-0004

## Context

Every feature must run at the lowest scalability tier, and presets are measured, not guessed
([Scalability](../architecture.md#scalability)). Both rules need fixed reference machines: a
frame-time budget means nothing without the hardware it is measured on. The same choice sets the
minimum GPU feature level, which the RHI and renderer depend on.

Constraints:

- The renderer is **bindless and GPU-driven** (GPU culling, indirect draws). Low-end machines are
  often CPU-bound, so this design helps them, but it needs bindless descriptor support.
- **Steam Deck** is a target device and the natural "potato" anchor: Zen 2, 4 cores / 8 threads;
  RDNA 2 GPU with 8 CUs at 1.6 GHz (1.6 TFLOPS FP32); 16 GB LPDDR5-6400 shared with the GPU;
  1280×800 display at up to 90 Hz ([Valve](https://www.steamdeck.com/en/tech)). It uses Vulkan
  through Mesa's RADV driver on Linux.
- A shipping game must run on drivers its vendor still maintains.

Evidence (Steam Hardware Survey, October 2026,
[GPUs](https://store.steampowered.com/hwsurvey/videocard/),
[overview](https://store.steampowered.com/hwsurvey/)):

- The most common GPUs are the RTX 3060 and RTX 5070 (3.78 % each), RTX 4060 Laptop (3.60 %),
  RTX 4060 (3.51 %), RTX 3050 (3.25 %) and RTX 5060 (3.15 %). The GTX 1650 is at 2.44 %, the
  GTX 1060 at 1.45 % and the GTX 1050 Ti at 1.14 %. Steam Deck GPUs total about 0.8 %.
- VRAM: 8 GB 26.9 %, 16 GB 26.4 %, 12 GB 12.9 %, 4 GB 5.5 %.
- RAM: 16 GB 41.4 %, 32 GB 37.1 %, 8 GB 7.5 %. CPU cores: 8 cores 28.7 %, 6 cores 27.3 %,
  4 cores 12.4 %.
- Display: 1920×1080 50.2 %, 2560×1440 21.8 %, 3840×2160 5.1 %.

Driver status of older GPUs:

- **NVIDIA Maxwell, Pascal and Volta** (GTX 900 and 10 series): Game Ready drivers ended with
  the 580 branch in October 2025. They now get only quarterly security updates, until October
  2028 ([Tom's Hardware](https://www.tomshardware.com/pc-components/gpus/nvidia-confirms-end-of-game-ready-driver-support-for-maxwell-and-pascal-gpus-affected-products-will-get-optimized-drivers-through-october-2025),
  [Windows Central](https://www.windowscentral.com/hardware/nvidia/nvidia-maxwell-pascal-volta-security-driver-update)).
  On Linux, the 590 branch dropped them. NVIDIA's open kernel modules, the default since 560,
  need Turing or newer ([NVIDIA](https://developer.nvidia.com/blog/nvidia-transitions-fully-towards-open-source-gpu-kernel-modules)).
- **AMD Polaris and Vega** (RX 400/500, Vega, Radeon VII): on a separate legacy branch since
  2023, with critical fixes only and no performance work
  ([AMD 26.5.2 release notes](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-26-5-2-POLARIS-VEGA.html)).

Bindless on Direct3D 12 means SM 6.6 dynamic resources (`ResourceDescriptorHeap`). They need
Resource Binding Tier 3 together with Shader Model 6.6
([spec](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html)).
Enhanced barriers depend on the driver, not on a hardware tier, and must be checked at run time
([Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/enhanced-barriers)).
Mesh shaders are part of feature level 12_2 and are not required by 12_1
([spec](https://microsoft.github.io/DirectX-Specs/d3d/D3D12_FeatureLevel12_2.html)).

## Options considered

**A. Pascal/Polaris baseline: GTX 1060 6 GB and RX 580 as the Low desktop tier** (the earlier
draft plan).
- Reaches the most old hardware.
- Both vendors have stopped optimizing drivers for these GPUs, and NVIDIA's Linux driver has
  dropped them. Bugs we find there will not be fixed.
- Without mesh shaders and with weaker bindless and async-compute behaviour, they need the most
  fallback work, for about 3 % of Steam GPUs, and that share is falling.

**B. Turing / RDNA 1 baseline, mesh shaders optional (chosen).**
- The minimum is a feature set: Vulkan 1.3 with bindless, or D3D12 with SM 6.6 and Resource
  Binding Tier 3. In practice that means NVIDIA GTX 16 or RTX 20 and newer, AMD RX 5000 and
  newer, Intel Arc, and Steam Deck. All of these get current drivers.
- The GPU-driven baseline is compute culling plus indirect draws, which works on all of them.
  Mesh shaders are a faster path where present.
- Keeps the GTX 1650 (the most common pre-RTX card, 4 GB) as the Low desktop reference.

**C. DirectX 12 Ultimate baseline: mesh shaders required** (RTX 20, RX 6000, Arc and newer;
Steam Deck qualifies).
- One geometry path, which is the simplest renderer.
- Excludes the GTX 16 series (if it lacks the cross-vendor mesh shader API), RX 5000 and many
  integrated GPUs. Their combined share is several percent and they are exactly the Low-tier
  market.
- Better taken later, when the virtualized-geometry work starts (see revisit triggers).

## Decision

1. **Minimum spec.** The engine starts only when the device has:
   - **Vulkan:** version 1.3 (dynamic rendering, synchronization2, maintenance4), timeline
     semaphores, buffer device address, `drawIndirectCount`, subgroup ballot and arithmetic in
     compute, and descriptor indexing with runtime arrays, partially bound and update-after-bind
     descriptors, and non-uniform indexing;
   - **Direct3D 12:** feature level 12_0, Shader Model 6.6, Resource Binding Tier 3, wave
     intrinsics and enhanced barriers. The Agility SDK ships with the game.

   Exact descriptor limits are fixed by the RHI ADR. Mesh shaders, hardware ray tracing,
   variable-rate shading and sampler feedback are **optional**. They are queried at start-up and
   enabled per tier.
2. **Supported GPU generations:** NVIDIA Turing (GTX 16, RTX 20) and newer; AMD RDNA 1
   (RX 5000) and newer, including RDNA 2/3 integrated GPUs and Steam Deck; Intel Arc (Xe-HPG)
   and newer.
3. **Excluded, untested and unsupported:**
   - NVIDIA Maxwell, Pascal and Volta, and anything older;
   - AMD GCN, including Polaris and Vega (and Vega integrated GPUs);
   - Intel integrated GPUs before Arc.

   The reason is vendor driver end of life, plus performance below the Low tier for most
   integrated GPUs. The start-up check gates on **features, not device IDs**. An excluded GPU
   that passes it runs with a "not supported" warning.
4. **Tiers and reference hardware.** Each tier's VRAM column is a budget, and the renderer must
   fit within it.

   | Tier | Reference GPU (NVIDIA / AMD) | VRAM budget | CPU | RAM | Output | Internal (upscaled) | Target |
   |---|---|---|---|---|---|---|---|
   | Low, handheld | Steam Deck (RDNA 2, 8 CU) | 4 GB of 16 GB shared | Zen 2, 4c/8t | 16 GB shared | 1280×800 | 50–67 % (640×400–853×533) | 30 fps; 40 fps at 40 Hz as an option |
   | Low, desktop | GTX 1650 4 GB / RX 6500 XT 4 GB | 4 GB | 4c/8t (Ryzen 3 3100, Core i3-10100) | 8 GB | 1920×1080 | 50–67 % (540p–720p) | 60 fps |
   | Medium | RTX 4060 8 GB / RX 7600 8 GB | 8 GB | 6 cores (Ryzen 5 5600, Core i5-12400) | 16 GB | 1920×1080 | 67–100 % | 60 fps |
   | High | RTX 5070 12 GB / RX 9070 16 GB | 12 GB | 8 cores (Ryzen 7 7700) | 32 GB | 2560×1440 | 67 % (960p) | 60 fps |
   | Epic | RTX 5080 16 GB / RX 9070 XT 16 GB, hardware RT | 16 GB | 8 cores (Ryzen 7 9800X3D) | 32 GB | 3840×2160 | 50–67 % (1080p–1440p) | 60 fps |

5. **A target is met when** the tier's preset, on its reference machine, keeps the 95th
   percentile frame time at or below the target (33.3 ms or 16.7 ms) on every benchmark camera
   path, within the VRAM budget, at the stated internal resolution and with every look-defining
   feature on.
6. **Benchmark lab** (self-hosted, nightly; hosted CI runners have no GPU):

   | Machine | Covers | OS and API |
   |---|---|---|
   | Steam Deck | Low handheld gate; AMD RDNA 2; Mesa | SteamOS, Vulkan |
   | Low PC: GTX 1650 4 GB, 4c/8t, 8 GB | Low desktop gate; minimum NVIDIA generation; 4 GB VRAM pressure | Windows, D3D12 and Vulkan |
   | Medium PC: RX 7600 8 GB, 6 cores, 16 GB | Medium gate; AMD Windows driver | Windows, D3D12 and Vulkan |
   | Epic PC: RTX 5080 16 GB, 8 cores, 32 GB | Epic gate; hardware RT; current NVIDIA | Windows and Linux, both APIs |

   - **The lab is phased in.** The Steam Deck and the Low PC come first, when the renderer
     draws its first scenes, because they gate the rule that every feature runs at Low. The
     Medium and Epic PCs join when the scalability presets are tuned. Until a tier's machine is
     in the lab, that tier is checked by hand on comparable hardware and has no automatic gate.
   - Until a High reference machine (RTX 5070 class) joins the lab, High is checked on the Epic
     PC at 1440p: by the relative rule (each preset saves at least the agreed share of frame
     time over the next one up) and by perceptual error, without an absolute gate.
   - Functional tests (not performance) run on hosted runners with software drivers: WARP for
     D3D12 and lavapipe for Vulkan.
7. **Review yearly** against the latest Steam Hardware Survey and vendor driver notices.

## Consequences

- The RHI targets a single, modern binding model on both APIs: no descriptor-table fallback and
  no legacy barriers. That removes a whole compatibility layer.
- The geometry pipeline keeps a compute-culling plus indirect-draw path as the baseline, and mesh
  shaders are an addition. That is two paths to maintain until the revisit below.
- About 3 % of Steam GPUs (GTX 10 series, RX 400/500) are no longer supported. Store pages state
  the minimum as "GTX 1650 / RX 5500 or newer, Vulkan 1.3 or D3D12 SM 6.6".
- 4 GB of VRAM at Low forces streaming budgets and texture pool sizing from the first renderer.
- Buying the lab is a cost: a Steam Deck, two mid-range PCs and one high-end PC. Phasing
  it in spreads that cost over the roadmap.
- Follow-up work:
  - the RHI ADR fixes descriptor limits and the capability query;
  - the Scalability Lab ADR fixes the benchmark paths and the relative-saving threshold;
  - on the lab machines, verify the feature checks the decision depends on: enhanced barriers
    on GTX 16 and RX 5000 drivers, and the mesh shader API on GTX 16.
- **Revisit when:**
  - the virtualized-geometry work starts (consider option C if GTX 16 and RX 5000 together are
    below 2 % of Steam GPUs);
  - Valve ships a successor to Steam Deck;
  - a console target is chosen;
  - the Low desktop reference falls below 1 % of Steam GPUs.
