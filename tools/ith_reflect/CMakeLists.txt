# ith_reflect: the reflection generator (ADR-0018). The scanner is a library so its tests can
# run it in-process; the executable is what the build runs (ith_reflect_target in
# cmake/IronthornModule.cmake).

add_library(ith_reflect_scanner STATIC
    Lexer.h
    Lexer.cpp
    Parser.h
    Parser.cpp
    Generator.h
    Generator.cpp)
add_library(ith::reflect_scanner ALIAS ith_reflect_scanner)
target_include_directories(ith_reflect_scanner PUBLIC "${PROJECT_SOURCE_DIR}/tools")
target_link_libraries(ith_reflect_scanner PUBLIC ith::build_options ith::foundation)
set_target_properties(ith_reflect_scanner PROPERTIES
    FOLDER "Ironthorn/Tools"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})

add_executable(ith_reflect Main.cpp)
target_link_libraries(ith_reflect PRIVATE ith::reflect_scanner)
set_target_properties(ith_reflect PROPERTIES
    FOLDER "Ironthorn/Tools"
    COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
