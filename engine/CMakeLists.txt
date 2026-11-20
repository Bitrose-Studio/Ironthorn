# Engine modules, lowest layer first. Each module lives in engine/ith/<name> and is declared
# with ith_add_module (cmake/IronthornModule.cmake). A module is created only when it has a
# defined responsibility and real code - no empty placeholders.

add_subdirectory(ith/foundation)
