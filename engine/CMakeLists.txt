# Engine modules, lowest layer first. Each module lives in engine/ith/<name> and is declared
# with ith_add_module (cmake/IronthornModule.cmake). A module is created only when it has a
# defined responsibility and real code - no empty placeholders.

add_subdirectory(ith/foundation)

# Tools-only modules: linked by the editor and the cooker, never by a game (ADR-0004, ADR-0011).
if(IRONTHORN_BUILD_TOOLS)
    add_subdirectory(ith/shader_compiler)
endif()
