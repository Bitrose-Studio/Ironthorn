# Engine modules, lowest layer first. Each module lives in engine/ith/<name> and is declared
# with ith_add_module (cmake/IronthornModule.cmake). A module is created only when it has a
# defined responsibility and real code - no empty placeholders. A module whose kind is turned
# off (IRONTHORN_BUILD_CLIENT, IRONTHORN_BUILD_TOOLS, IRONTHORN_BUILD_EDITOR) is skipped by
# ith_add_module itself.

add_subdirectory(ith/foundation)
add_subdirectory(ith/math)
add_subdirectory(ith/file_system)
add_subdirectory(ith/platform_events)
add_subdirectory(ith/platform)
add_subdirectory(ith/jobs)
add_subdirectory(ith/rhi)
add_subdirectory(ith/rhi_d3d12)
add_subdirectory(ith/rhi_vulkan)
add_subdirectory(ith/rhi_select)
add_subdirectory(ith/reflection)
add_subdirectory(ith/serialization)
add_subdirectory(ith/config)
add_subdirectory(ith/ecs)
add_subdirectory(ith/level)
add_subdirectory(ith/shapes)
add_subdirectory(ith/assets)
add_subdirectory(ith/input)
add_subdirectory(ith/input_actions)
add_subdirectory(ith/gameplay)
add_subdirectory(ith/render_graph)
# The shader compiler needs Slang and DXC, which sanitizer builds leave out (ITH_BUILD_SHADER_COMPILER).
if(ITH_BUILD_SHADER_COMPILER)
    add_subdirectory(ith/shader_compiler)
endif()
add_subdirectory(ith/asset_database)
add_subdirectory(ith/asset_importers)
add_subdirectory(ith/editor_core)
add_subdirectory(ith/editor_ui)

# The engine's configuration layer (ADR-0035 rule 12): engine/config is copied to Config/ next to
# the executables of every configuration, so applications find it at <exe>/Config/. Files removed
# from engine/config disappear from the copy at the next build.
file(GLOB_RECURSE ith_engine_config_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config/*")
set(ith_engine_config_stamp "${CMAKE_BINARY_DIR}/engine_config/$<CONFIG>/copied.stamp")
add_custom_command(
    OUTPUT "${ith_engine_config_stamp}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/config" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Config"
    COMMAND ${CMAKE_COMMAND} -E touch "${ith_engine_config_stamp}"
    DEPENDS ${ith_engine_config_files}
    COMMENT "Copying the engine configuration next to the executables"
    VERBATIM)
add_custom_target(ith_engine_config ALL DEPENDS "${ith_engine_config_stamp}")
set_target_properties(ith_engine_config PROPERTIES FOLDER "Ironthorn/Engine")
