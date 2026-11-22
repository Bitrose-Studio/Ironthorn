# Enforces ADR-0029 rule 2: Dear ImGui stays inside the editor_ui module, so replacing the toolkit
# means rewriting editor_ui only.
# - Only source files of engine/ith/editor_ui include ImGui (imgui.h, imgui_internal.h, imconfig.h,
#   the bundled imstb_*.h, the backends), and none of its headers does: no public header names an
#   ImGui type.
# - imgui_internal.h, which holds the docking internals, is included by engine/ith/editor_ui/DockLayout.cpp
#   only.
# - Only engine/ith/editor_ui/CMakeLists.txt links ImGui (ith::imgui or ith_imgui) among the
#   CMakeLists.txt files searched.
#
# Usage:
#   cmake -DREPOSITORY_DIR=<repo> [-DDIRECTORIES="engine,samples,tools,editor"] -P CheckImGuiConfined.cmake
#
# DIRECTORIES (relative to REPOSITORY_DIR, separated by commas) are searched recursively; missing
# ones are ignored. Tests are not searched: headless editor_ui tests drive ImGui directly.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR)
    message(FATAL_ERROR "REPOSITORY_DIR must be defined.")
endif()
if(NOT DEFINED DIRECTORIES)
    set(DIRECTORIES "engine,samples,tools,editor")
endif()
string(REPLACE "," ";" directories "${DIRECTORIES}")

set(module "engine/ith/editor_ui")
set(dock_layout "${module}/DockLayout.cpp")
set(include_pattern "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]*/)?(imgui[^/>\"]*\\.h|imconfig\\.h|imstb_[^/>\"]*\\.h)[>\"]")

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
        math(EXPR count "${count} + 1")
        file(STRINGS "${REPOSITORY_DIR}/${source}" includes REGEX "${include_pattern}")
        foreach(line IN LISTS includes)
            if(NOT line MATCHES "${include_pattern}")
                continue()
            endif()
            set(header "${CMAKE_MATCH_2}")
            if(header STREQUAL "imgui_internal.h" AND NOT source STREQUAL "${dock_layout}")
                list(APPEND violations "${source} includes ${header}: only ${dock_layout} may use ImGui's internals")
            elseif(NOT source MATCHES "^${module}/")
                list(APPEND violations "${source} includes ${header}: only the editor_ui module may include ImGui")
            elseif(source MATCHES "\\.(h|hpp|inl)$")
                list(APPEND violations "${source} includes ${header}: editor_ui's headers must not name ImGui types")
            endif()
        endforeach()
    endforeach()

    file(GLOB_RECURSE lists RELATIVE "${REPOSITORY_DIR}" "${REPOSITORY_DIR}/${directory}/CMakeLists.txt")
    foreach(list_file IN LISTS lists)
        if(list_file STREQUAL "${module}/CMakeLists.txt")
            continue()
        endif()
        file(STRINGS "${REPOSITORY_DIR}/${list_file}" links REGEX "ith(::|_)imgui([^A-Za-z0-9_]|$)")
        if(links)
            list(APPEND violations "${list_file} links ImGui: only the editor_ui module may")
        endif()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "ImGui outside the editor_ui module (ADR-0029 rule 2):\n  ${text}")
endif()
message(STATUS "ImGui confined: ${count} file(s) checked.")
