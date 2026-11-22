# Enforces ADR-0035 rule 10: file I/O under engine/ith goes through the file_system module. A
# source file outside engine/ith/file_system that includes <filesystem> or <fstream>, or calls
# fopen or CreateFileW, is a violation.
#
# Usage:
#   cmake -DREPOSITORY_DIR=<repo>
#         [-DDIRECTORIES="engine/ith"]
#         -P CheckFileIo.cmake
#
# DIRECTORIES (relative to REPOSITORY_DIR, separated by commas) are searched recursively for
# .h, .hpp, .inl, .c and .cpp files; missing ones are ignored. engine/ith/file_system is always
# allowed. Tests, tools and samples are not checked.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR)
    message(FATAL_ERROR "REPOSITORY_DIR must be defined.")
endif()
if(NOT DEFINED DIRECTORIES)
    set(DIRECTORIES "engine/ith")
endif()
string(REPLACE "," ";" directories "${DIRECTORIES}")

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
        if(source MATCHES "^engine/ith/file_system/")
            continue()
        endif()
        math(EXPR count "${count} + 1")
        file(STRINGS "${REPOSITORY_DIR}/${source}" includes
            REGEX "^[ \t]*#[ \t]*include[ \t]*<(filesystem|fstream)>")
        foreach(line IN LISTS includes)
            if(line MATCHES "<(filesystem|fstream)>")
                list(APPEND violations "${source} includes <${CMAKE_MATCH_1}>; use ith/file_system instead")
            endif()
        endforeach()
        file(STRINGS "${REPOSITORY_DIR}/${source}" calls REGEX "(^|[^A-Za-z0-9_])(fopen|CreateFileW)[ \t]*\\(")
        foreach(line IN LISTS calls)
            if(line MATCHES "(^|[^A-Za-z0-9_])(fopen|CreateFileW)[ \t]*\\(")
                list(APPEND violations "${source} calls ${CMAKE_MATCH_2}; use ith/file_system instead")
            endif()
        endforeach()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "File I/O outside the file_system module (ADR-0035 rule 10):\n  ${text}")
endif()
message(STATUS "file I/O: ${count} file(s) outside engine/ith/file_system clean.")
