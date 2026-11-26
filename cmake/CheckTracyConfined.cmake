# Enforces ADR-0007 rules 10 to 12: engine and game code profile through the ITH_PROFILE_* macros,
# so replacing the backend means rewriting ith/foundation/Profile.h (and the GPU zones in the RHI
# backends) only.
# - Only the files in `allowed` include Tracy (<tracy/...>, "tracy/...", or a Tracy*.hpp header).
#   Today that is engine/ith/foundation/Profile.h; the private GPU-zone sources of the Vulkan and
#   Direct3D 12 backends join the list when they exist (rule 12).
# - Only engine/ith/foundation/CMakeLists.txt links Tracy (ith::tracy or ith_tracy) among the
#   CMakeLists.txt files searched.
#
# Usage:
#   cmake -DREPOSITORY_DIR=<repo>
#         [-DDIRECTORIES="engine,samples,tests,tools,editor"]
#         [-DSKIP="tests/layering"]
#         -P CheckTracyConfined.cmake
#
# DIRECTORIES (relative to REPOSITORY_DIR, separated by commas) are searched recursively; missing
# ones are ignored. SKIP lists directories left out (lint fixtures that exist to be refused).

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR)
    message(FATAL_ERROR "REPOSITORY_DIR must be defined.")
endif()
if(NOT DEFINED DIRECTORIES)
    set(DIRECTORIES "engine,samples,tests,tools,editor")
endif()
string(REPLACE "," ";" directories "${DIRECTORIES}")
string(REPLACE "," ";" skipped "${SKIP}")

set(allowed "engine/ith/foundation/Profile.h")
set(linker "engine/ith/foundation/CMakeLists.txt")
set(include_pattern "^[ \t]*#[ \t]*include[ \t]*[<\"](tracy/[^>\"]*|([^>\"]*/)?Tracy[^/>\"]*\\.hpp)[>\"]")

set(violations "")
set(count 0)
foreach(directory IN LISTS directories)
    if(NOT IS_DIRECTORY "${REPOSITORY_DIR}/${directory}")
        continue()
    endif()
    file(GLOB_RECURSE sources RELATIVE "${REPOSITORY_DIR}"
        "${REPOSITORY_DIR}/${directory}/*.h" "${REPOSITORY_DIR}/${directory}/*.hpp"
        "${REPOSITORY_DIR}/${directory}/*.inl" "${REPOSITORY_DIR}/${directory}/*.c"
        "${REPOSITORY_DIR}/${directory}/*.cpp" "${REPOSITORY_DIR}/${directory}/CMakeLists.txt")
    foreach(source IN LISTS sources)
        set(skip FALSE)
        foreach(skipped_directory IN LISTS skipped)
            if(source MATCHES "^${skipped_directory}/")
                set(skip TRUE)
            endif()
        endforeach()
        if(skip)
            continue()
        endif()
        math(EXPR count "${count} + 1")
        if(source MATCHES "CMakeLists\\.txt$")
            if(NOT source STREQUAL "${linker}")
                file(STRINGS "${REPOSITORY_DIR}/${source}" links REGEX "ith(::|_)tracy([^A-Za-z0-9_]|$)")
                if(links)
                    list(APPEND violations "${source} links Tracy: only the foundation module may")
                endif()
            endif()
            continue()
        endif()
        if(source IN_LIST allowed)
            continue()
        endif()
        file(STRINGS "${REPOSITORY_DIR}/${source}" includes REGEX "${include_pattern}")
        foreach(line IN LISTS includes)
            if(line MATCHES "${include_pattern}")
                list(APPEND violations "${source} includes ${CMAKE_MATCH_1}: profile through ith/foundation/Profile.h")
            endif()
        endforeach()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "Tracy outside ith/foundation/Profile.h (ADR-0007):\n  ${text}")
endif()
message(STATUS "Tracy confined: ${count} file(s) checked.")
