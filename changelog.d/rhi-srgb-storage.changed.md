- `rhi::Device::createTexture()` refuses a texture with `TextureUsage::Storage` and an sRGB format
  on every backend, with `rhi.invalid_usage`.
