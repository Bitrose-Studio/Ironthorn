- The `scene_player` sample draws through the RHI's bindless model: 10,000 instanced cubes in one
  indexed draw with a depth buffer (the default), or a triangle (`--scene triangle`), on
  Direct3D 12 or Vulkan (`--backend`, `--adapter`, `--frames`, `--no-vsync`), and prints its frame
  timings. It is built with the tools, which compile its shaders.
- `ith_embed_shaders()` compiles Slang entry points at build time with the pinned `slangc` into
  headers a sample or test embeds, as SPIR-V and optionally DXIL.
- The `scene_player` GPU test renders the sample's scenes offscreen on every backend the machine
  has and checks them against the geometry, and compares Direct3D 12 with Vulkan.
