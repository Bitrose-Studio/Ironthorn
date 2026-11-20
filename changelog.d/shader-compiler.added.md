- `ith/shader_compiler/ShaderCompiler.h`: `ith::ShaderCompiler`, a tools-only module that compiles
  Slang shader source to SPIR-V 1.6 and to signed DXIL Shader Model 6.6 through Slang 2026.22.1
  and DXC 1.9.2609, reporting compiler diagnostics through `ith::Expected`.
- The CMake option `IRONTHORN_BUILD_TOOLS` (on by default) builds the tools-only modules and
  downloads their pinned dependencies.
