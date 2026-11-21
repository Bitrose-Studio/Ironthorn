# Shaders

Shaders are written in Slang and compiled to SPIR-V for Vulkan and to DXIL for Direct3D 12
([ADR-0011](../decisions/0011-shader-language-and-pipeline.md)). Two paths compile them, and both
use the options below:

- the `shader_compiler` tools module (`ith::ShaderCompiler`, `engine/ith/shader_compiler`), for
  the editor, the cooker and the RHI backend tests;
- `ith_embed_shaders()` (`cmake/IronthornDependencies.cmake`), which runs the pinned `slangc` at
  build time for shaders that samples and engine tests embed, such as `scene_player`'s
  ([ADR-0023](../decisions/0023-build-time-shaders-for-samples-and-tests.md)). Games, engine
  modules and engine built-in shaders don't use it.

## Compile options

This is the one list of the engine's shader compile options. A change to it changes
`ShaderCompiler::compile()` (`ShaderCompiler.cpp`) and `ith_embed_shaders()` in the same commit;
comments at both point here.

| Option | SPIR-V | DXIL | `ShaderCompiler` | `slangc` |
|---|---|---|---|---|
| Target and profile | SPIR-V 1.6 (Vulkan 1.3) | Shader Model 6.6 in a DXBC container | `SLANG_SPIRV` / `SLANG_DXIL`, profile `spirv_1_6` / `sm_6_6` | `-target spirv -profile spirv_1_6` / `-target dxil -profile sm_6_6` |
| Matrix layout | column-major (ADR-0006) | column-major | `SLANG_MATRIX_LAYOUT_COLUMN_MAJOR` | `-matrix-layout-column-major` |
| Validation | checked by the SPIR-V tools of Slang's glslang library | validated and signed by DXC's validator | `ValidateWithDownstreamCompiler` (SPIR-V) | `-validate-with-downstream-compiler` (SPIR-V) |
| Entry point name | kept, not renamed to `main` | kept | `VulkanUseEntryPointName` | `-fvk-use-entrypoint-name` |
| Defines | `ITH_SPIRV=1` | none | a preprocessor macro of the session | `-D ITH_SPIRV=1` |
| DXC | | the pinned DXC of ADR-0011 | loaded by Slang from its own directory, where the build copies DXC | `-dxc-path` with the pinned DXC's directory |

Both use only the pinned Slang and DXC, never a compiler from `PATH` or an SDK.

## One source for both APIs

Shaders reach every resource through bindless indices (ADR-0009). Until the engine's bindless
Slang module exists, a source tells the APIs apart with `ITH_SPIRV`:

```slang
#ifdef ITH_SPIRV
[[vk::binding(0, 0)]] ByteAddressBuffer buffers[];   // the RHI's bindless set, binding 0
ByteAddressBuffer buffer(uint index) { return buffers[index]; }
#else
ByteAddressBuffer buffer(uint index) { return ResourceDescriptorHeap[index]; }
#endif
```

Push constants are declared as `[[vk::push_constant]] ConstantBuffer<T>`, which DXIL sees as the
constant buffer at `b0, space0` ([RHI guide](rhi.md#vulkan)).

## Embedding shaders at build time

```cmake
ith_embed_shaders(<target> SOURCE Cubes.slang ENTRY_POINTS cubeVertex:vertex cubePixel:pixel [DXIL])
```

writes `cubeVertex.spirv.h` (and with `DXIL` `cubeVertex.dxil.h`) into a directory of the target
in the build tree, each defining `const unsigned char kCubeVertexSpirv[]` and its size
`kCubeVertexSpirv_sizeInBytes`. Include them inside a namespace that says `using std::size_t;`.
A header is rebuilt when its source, a file the source imports or includes (Slang's depfile),
`slangc` or, for DXIL, DXC changes; a compile error fails the build. The targets that use it are
built only with `IRONTHORN_BUILD_TOOLS`.
