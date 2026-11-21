# Engine modules, lowest layer first. Each module lives in engine/ith/<name> and is declared
# with ith_add_module (cmake/IronthornModule.cmake). A module is created only when it has a
# defined responsibility and real code - no empty placeholders. A module whose kind is turned
# off (IRONTHORN_BUILD_CLIENT, IRONTHORN_BUILD_TOOLS, IRONTHORN_BUILD_EDITOR) is skipped by
# ith_add_module itself.

add_subdirectory(ith/foundation)
add_subdirectory(ith/reflection)
add_subdirectory(ith/ecs)
add_subdirectory(ith/shader_compiler)
