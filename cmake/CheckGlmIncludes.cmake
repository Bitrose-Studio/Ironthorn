# Enforces ADR-0006 rule 2: engine code reaches glm only through the math module's headers
# ("ith/math/..."), so no file sees glm without the engine's configuration, and the ith aliases
# stay the engine's vocabulary. A source file outside engine/ith/math that includes <glm/...> or
# "glm/..." is a violation.
#
# Usage:
#   cmake -DREPOSITORY_DIR=<repo>
#         [-DDIRECTORIES="engine,samples,tests,tools,editor"]
#         [-DSKIP="tests/layering"]
#         -P CheckGlmIncludes.cmake
#
# DIRECTORIES (relative to REPOSITORY_DIR, separated by commas) are searched recursively for
# .h, .hpp, .inl, .c and .cpp files; missing ones are ignored. SKIP lists directories left out
# (lint fixtures that exist to be refused). engine/ith/math is always allowed.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR)
    message(FATAL_ERROR "REPOSITORY_DIR must be defined.")
endif()
if(NOT DEFINED DIRECTORIES)
    set(DIRECTORIES "engine,samples,tests,tools,editor")
endif()
string(REPLACE "," ";" directories "${DIRECTORIES}")
string(REPLACE "," ";" skipped "${SKIP}")
list(APPEND skipped "engine/ith/math")

set(violations "")
set(count 0)
foreach(directory IN LISTS directories)
    if(NOT IS_DIRECTORY "${REPOSITORY_DIR}/${directory}")
        continue()
    endif()
    file(GLOB_RECURSE sources RELATIVE "${REPOSITORY_DIR}"
        "${REPOSITORY_DIR}/${directory}/*.h" "${REPOSITORY_DIR}/${directory}/*.hpp"
        "${REPOSITORY_DIR}/${directory}/*.inl" "${REPOSITORY_DIR}/${directory}/*.c"
        "${REPOSITORY_DIR}/${directory}/*.cpp")
    foreach(source IN LISTS sources)
        set(allowed FALSE)
        foreach(skip IN LISTS skipped)
            if(source MATCHES "^${skip}/")
                set(allowed TRUE)
            endif()
        endforeach()
        if(allowed)
            continue()
        endif()
        math(EXPR count "${count} + 1")
        file(STRINGS "${REPOSITORY_DIR}/${source}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]glm/")
        foreach(line IN LISTS includes)
            if(line MATCHES "([<\"]glm/[^>\"]*[>\"])")
                list(APPEND violations "${source} includes ${CMAKE_MATCH_1}; include an ith/math header instead")
            endif()
        endforeach()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "Direct glm includes (ADR-0006 rule 2):\n  ${text}")
endif()
message(STATUS "glm includes: ${count} file(s) outside engine/ith/math clean.")
