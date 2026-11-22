- Texture copies take a texel region: `rhi::BufferTextureCopy::region` copies a box of a mip level
  instead of the whole level, on Direct3D 12, Vulkan and the null backend. The default region is
  still the whole level.
