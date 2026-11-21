- The shader compiler defines `ITH_SPIRV=1` when it compiles SPIR-V, as `ith_embed_shaders()` does,
  so one Slang source can declare the Vulkan bindless set for SPIR-V and use
  `ResourceDescriptorHeap` for DXIL. The engine's shader compile options are listed in
  `docs/development/shaders.md`.
