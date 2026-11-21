#pragma once

// Compiles Slang shader source to SPIR-V for Vulkan and to DXIL for Direct3D 12 (ADR-0011).
//
// This is a tools-only module: the editor and the cooker link it, while games load precompiled
// bytecode and never contain a compiler. Compilation runs in-process through Slang's library
// API; DXIL is produced by DXC, which Slang loads from the directory of its own library.
//
//   ith::Expected<ith::ShaderCompiler> compiler = ith::ShaderCompiler::create();
//   if (!compiler) {
//       return ith::makeUnexpected(compiler.error());
//   }
//   ith::Expected<std::vector<std::byte>> spirv = compiler->compile({
//       .name = "Fill.slang",
//       .source = text,
//       .entryPoint = "main",
//       .stage = ith::ShaderStage::Compute,
//       .target = ith::ShaderTarget::Spirv,
//   });
//
// Code is generated with column-major matrix layout, as SPIR-V 1.6 and as DXIL Shader Model 6.6.
// SPIR-V keeps the entry point's name (not "main"), and is optimized and checked by the SPIR-V
// tools in Slang's glslang library, without which it can't be generated; DXIL is validated and
// signed by DXC. SPIR-V compilations define ITH_SPIRV=1, so that one source can declare the
// Vulkan bindless set for SPIR-V and use ResourceDescriptorHeap for DXIL. The options are listed in
// docs/development/shaders.md.

#include "ith/foundation/Expected.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ith {

// The pipeline stage an entry point is compiled for.
enum class ShaderStage : std::uint8_t {
    Vertex,
    Pixel,
    Compute,
};

// The bytecode format to generate.
enum class ShaderTarget : std::uint8_t {
    Spirv, // SPIR-V 1.6, for Vulkan 1.3.
    Dxil,  // DXIL Shader Model 6.6 in a signed DXBC container, for Direct3D 12.
};

// What to compile. The views are only read during ShaderCompiler::compile().
struct ShaderRequest {
    // The name of the source in diagnostics, e.g. "passes/Tonemap.slang". It is also the Slang
    // module name, but only for this one compilation.
    std::string_view name;
    // The Slang source text. It must be self-contained for now: search paths for `import` and
    // `#include` come with the shader library.
    std::string_view source;
    // The function to compile. It doesn't need a [shader("...")] attribute.
    std::string_view entryPoint;
    ShaderStage stage = ShaderStage::Compute;
    ShaderTarget target = ShaderTarget::Spirv;
};

// Compiles Slang source with one Slang global session, created once and reused, because
// creating it loads Slang's core module and takes a noticeable fraction of a second.
//
// Not thread-safe: one ShaderCompiler must not be used from several threads at once. Separate
// ShaderCompiler objects may compile in parallel.
class ShaderCompiler {
public:
    // Starts the Slang compiler. Fails with "shader_compiler.unavailable" when the Slang library
    // can't be initialized.
    static Expected<ShaderCompiler> create();

    ShaderCompiler(ShaderCompiler&& other) noexcept;
    ShaderCompiler& operator=(ShaderCompiler&& other) noexcept;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;
    ~ShaderCompiler();

    // Compiles request.entryPoint of request.source for request.stage into request.target
    // bytecode, which the caller owns.
    //
    // Fails with "shader_compiler.compile_failed" when the source doesn't compile, when the entry
    // point is missing or isn't valid for the stage, when the generated code fails validation, or
    // when DXC isn't available for DXIL. The error message holds the compiler's diagnostics, which
    // name request.name and the line. Fails with "shader_compiler.unavailable" when Slang can't
    // create a compilation session. Warnings of a successful compilation are not reported yet.
    //
    // Precondition (asserted): the compiler hasn't been moved from.
    Expected<std::vector<std::byte>> compile(const ShaderRequest& request);

private:
    struct Impl;

    explicit ShaderCompiler(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace ith
