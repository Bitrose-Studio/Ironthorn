# The editor's UI on Dear ImGui (ADR-0029): the ImGui context with our own platform backend (engine
# events, clipboard, IME and cursor through ith::Platform) and renderer backend (render graph
# passes on the RHI), the theme, fonts and icons, the idle policy and the shell. ImGui is private to
# this module: it links ith::imgui privately, and no public header names an ImGui type (the
# imgui_confined test). The UI shader is compiled into the module as text and compiled to bytecode
# by the editor application, so this module builds without the shader compiler, in sanitizer
# builds too (ADR-0029 rule 19).
ith_add_module(editor_ui
    LAYER editor
    KIND editor
    SOURCES
        DockLayout.h
        DockLayout.cpp
        EditorShell.h
        EditorShell.cpp
        EditorUi.h
        EditorUi.cpp
        EditorUiShader.h
        EditorUiShader.cpp
        Icons.h
        Icons.cpp
        IdlePolicy.h
        ImGuiConfig.h
        ImGuiPlatform.h
        ImGuiPlatform.cpp
        ImGuiRenderer.h
        ImGuiRenderer.cpp
        OutputLog.h
        OutputLog.cpp
        Theme.h
        Theme.cpp
    DEPENDS
        foundation
        file_system
        platform_events
        platform
        input
        rhi
        render_graph
        serialization
        editor_core
    PRIVATE_LIBRARIES
        ith::imgui)

# Without the editor modules (IRONTHORN_BUILD_EDITOR off), ith_add_module declared nothing.
if(NOT TARGET ith_editor_ui)
    return()
endif()

# The shader source as a string literal. CMake configures again when the shader changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/EditorUi.slang")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/EditorUi.slang" ITH_EDITOR_UI_SHADER_SOURCE)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/shaders/EditorUiShaderSource.inl.in"
    "${PROJECT_BINARY_DIR}/generated/editor_ui/EditorUiShaderSource.inl" @ONLY)

# The editor fonts and their licences, copied to fonts/ next to the executables of every
# configuration (EditorUiDesc::fontDirectory), as the engine configuration is copied to Config/.
# Editor distributions ship this folder; the licences are OFL-1.1 for Inter and JetBrains Mono, and
# ISC with MIT for Lucide (docs/development/third-party.md).
set(ith_editor_font_files
    "${font_inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
    "${font_inter_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf"
    "${font_jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf"
    "${font_lucide_SOURCE_DIR}/lucide.ttf")
foreach(file IN LISTS ith_editor_font_files ITH_LUCIDE_LICENSE)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The editor font archives have no ${file}; correct cmake/IronthornDependencies.cmake.")
    endif()
endforeach()
set(ith_editor_fonts_dir "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/fonts")
set(ith_editor_fonts_stamp "${CMAKE_BINARY_DIR}/editor_fonts/$<CONFIG>/copied.stamp")
add_custom_command(
    OUTPUT "${ith_editor_fonts_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ith_editor_fonts_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ith_editor_font_files} "${ith_editor_fonts_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${font_inter_SOURCE_DIR}/LICENSE.txt" "${ith_editor_fonts_dir}/Inter-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${font_jetbrains_mono_SOURCE_DIR}/OFL.txt" "${ith_editor_fonts_dir}/JetBrainsMono-OFL.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ITH_LUCIDE_LICENSE}" "${ith_editor_fonts_dir}/Lucide-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E touch "${ith_editor_fonts_stamp}"
    DEPENDS ${ith_editor_font_files} ${ITH_LUCIDE_LICENSE}
    COMMENT "Copying the editor fonts next to the executables"
    VERBATIM)
add_custom_target(ith_editor_fonts ALL DEPENDS "${ith_editor_fonts_stamp}")
set_target_properties(ith_editor_fonts PROPERTIES FOLDER "Ironthorn/Engine")
add_dependencies(ith_editor_ui ith_editor_fonts)
