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
add_subdirectory(ith/ecs)
add_subdirectory(ith/level)
add_subdirectory(ith/assets)
add_subdirectory(ith/input)
add_subdirectory(ith/input_actions)
add_subdirectory(ith/render_graph)
# The shader compiler needs Slang and DXC, which sanitizer builds leave out (ITH_BUILD_SHADER_COMPILER).
if(ITH_BUILD_SHADER_COMPILER)
    add_subdirectory(ith/shader_compiler)
endif()
add_subdirectory(ith/asset_database)
add_subdirectory(ith/asset_importers)
