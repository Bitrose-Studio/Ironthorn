#pragma once

// The editor UI's shader source (ADR-0029 rule 9): editor_ui/shaders/EditorUi.slang, compiled
// into the module as text. The editor application compiles it at start-up with the shader
// compiler, for the device's bytecode format, and hands the result to EditorUiDesc; editor_ui
// doesn't link the compiler, so it builds and tests where the compiler is left out (sanitizer
// builds), on the null device, which takes any bytecode.
//
//   ith::Expected<std::vector<std::byte>> vertex = compiler.compile({
//       .name = ith::kEditorUiShaderName,
//       .source = ith::editorUiShaderSource(),
//       .entryPoint = ith::kEditorUiVertexEntry,
//       .stage = ith::ShaderStage::Vertex,
//       .target = target,
//   });

#include <cstdint>
#include <string_view>

namespace ith {

// The name of the source in compiler diagnostics.
inline constexpr std::string_view kEditorUiShaderName = "editor_ui/shaders/EditorUi.slang";

// The entry points of the vertex and pixel stages.
inline constexpr std::string_view kEditorUiVertexEntry = "editorUiVertex";
inline constexpr std::string_view kEditorUiPixelEntry = "editorUiPixel";

// Bytes of push constants both stages declare.
inline constexpr std::uint32_t kEditorUiConstantsSize = 32;

// The Slang source text, valid for the program's lifetime.
[[nodiscard]] std::string_view editorUiShaderSource() noexcept;

} // namespace ith
