#pragma once

// The viewport's shader sources (ADR-0042 rule 17): editor_ui/shaders/Shapes.slang (the shapes
// pass) and editor_ui/shaders/Grid.slang (the grid pass), compiled into the module as text. As with
// the UI shader (EditorUiShader.h), the editor application compiles them at start-up with the shader
// compiler, for the device's bytecode format, and hands the result to ViewportPanel::create();
// editor_ui doesn't link the compiler. Neither shader has variants.
//
//   ith::Expected<std::vector<std::byte>> vertex = compiler.compile({
//       .name = ith::kViewportShapesShaderName,
//       .source = ith::viewportShapesShaderSource(),
//       .entryPoint = ith::kViewportShapesVertexEntry,
//       .stage = ith::ShaderStage::Vertex,
//       .target = target,
//   });

#include <cstdint>
#include <string_view>

namespace ith {

// The shapes pass: its name in compiler diagnostics, its entry points, and the bytes of push
// constants both stages declare.
inline constexpr std::string_view kViewportShapesShaderName = "editor_ui/shaders/Shapes.slang";
inline constexpr std::string_view kViewportShapesVertexEntry = "shapesVertex";
inline constexpr std::string_view kViewportShapesPixelEntry = "shapesPixel";
inline constexpr std::uint32_t kViewportShapesConstantsSize = 128;

// The grid pass: its name in compiler diagnostics, its entry points, and the bytes of push constants
// its pixel stage declares (its vertex stage declares none).
inline constexpr std::string_view kViewportGridShaderName = "editor_ui/shaders/Grid.slang";
inline constexpr std::string_view kViewportGridVertexEntry = "gridVertex";
inline constexpr std::string_view kViewportGridPixelEntry = "gridPixel";
inline constexpr std::uint32_t kViewportGridConstantsSize = 80;

// The Slang source texts, valid for the program's lifetime.
[[nodiscard]] std::string_view viewportShapesShaderSource() noexcept;
[[nodiscard]] std::string_view viewportGridShaderSource() noexcept;

} // namespace ith
