# Enforces the module layering rules (ADR-0004) and the module kind rules (ADR-0017):
#   1. A module depends only on modules of the same or a lower layer, and dependencies have no cycles.
#   2. A module includes only its own headers and those of modules in its dependency closure.
#   3. A module's kind fits its layer: runtime and client modules live in the layers foundation
#      to framework, tools and editor modules in the editor layer.
#   4. A module depends only on modules of an allowed kind: runtime -> runtime; client -> runtime,
#      client; tools -> runtime, tools; editor -> any kind.
#   5. The full dependency closure of an application has only the kinds its target allows:
#      editor any; player runtime, client; server runtime; tools runtime, tools. An application
#      includes only headers of modules in that closure; its sources are listed in the table.
#
# Usage:
#   cmake -DREPOSITORY_DIR=<repo>
#         -DMODULE_TABLE="foundation:foundation:runtime=|ecs:core:runtime=foundation"
#         -DAPPLICATION_TABLE="minimal_server:server:samples/minimal_server/Main.cpp=ecs"
#         -DLAYERS="foundation,platform,core,..."
#         [-DUNAVAILABLE_MODULES="rhi_d3d12,..."]
#         -P CheckModuleLayering.cmake
#
# MODULE_TABLE entries are <module>:<layer>:<kind>=<dependencies>; APPLICATION_TABLE (optional)
# entries are <application>:<target>:<sources>=<modules>, with the sources relative to
# REPOSITORY_DIR and separated by commas. UNAVAILABLE_MODULES (optional) lists modules that exist
# only on other platforms, which a module or application may include behind a platform check.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR OR NOT DEFINED MODULE_TABLE OR NOT DEFINED LAYERS)
    message(FATAL_ERROR "REPOSITORY_DIR, MODULE_TABLE and LAYERS must be defined.")
endif()
set(ENGINE_DIR "${REPOSITORY_DIR}/engine/ith")

string(REPLACE "," ";" layers "${LAYERS}")
string(REPLACE "|" ";" entries "${MODULE_TABLE}")
string(REPLACE "|" ";" application_entries "${APPLICATION_TABLE}")
string(REPLACE "," ";" unavailable_modules "${UNAVAILABLE_MODULES}")

# The kinds each module kind may depend on, and each application target may link.
set(allowed_kinds_runtime runtime)
set(allowed_kinds_client runtime client)
set(allowed_kinds_tools runtime tools)
set(allowed_kinds_editor runtime client tools editor)
set(target_kinds_editor runtime client tools editor)
set(target_kinds_player runtime client)
set(target_kinds_server runtime)
set(target_kinds_tools runtime tools)

list(FIND layers "editor" editor_rank)

set(modules "")
foreach(entry IN LISTS entries)
    if(NOT entry MATCHES "^([a-z0-9_]+):([a-z]+):([a-z]+)=(.*)$")
        message(FATAL_ERROR "Malformed module table entry: '${entry}'")
    endif()
    set(module "${CMAKE_MATCH_1}")
    list(APPEND modules ${module})
    set(layer_${module} "${CMAKE_MATCH_2}")
    list(FIND layers "${CMAKE_MATCH_2}" rank_${module})
    set(kind_${module} "${CMAKE_MATCH_3}")
    string(REPLACE "," ";" direct_${module} "${CMAKE_MATCH_4}")
endforeach()

set(violations "")

# Rules 1, 3 and 4: the layer and kind of each module and of its dependencies.
foreach(module IN LISTS modules)
    set(kind "${kind_${module}}")
    if(NOT DEFINED allowed_kinds_${kind})
        list(APPEND violations "${module} has unknown kind '${kind}'")
        continue()
    endif()
    if(kind STREQUAL "runtime" OR kind STREQUAL "client")
        if(NOT rank_${module} LESS editor_rank)
            list(APPEND violations
                "${module} is a ${kind} module in the ${layer_${module}} layer, but ${kind} modules belong in the layers below editor")
        endif()
    elseif(NOT rank_${module} EQUAL editor_rank)
        list(APPEND violations
            "${module} is a ${kind} module in the ${layer_${module}} layer, but ${kind} modules belong in the editor layer")
    endif()

    foreach(dependency IN LISTS direct_${module})
        if(NOT dependency IN_LIST modules)
            list(APPEND violations "${module} depends on unknown module '${dependency}'")
            continue()
        endif()
        if(rank_${dependency} GREATER rank_${module})
            list(APPEND violations
                "${module} (layer ${rank_${module}}) depends on ${dependency} (layer ${rank_${dependency}}), a higher layer")
        endif()
        if(NOT kind_${dependency} IN_LIST allowed_kinds_${kind})
            list(JOIN allowed_kinds_${kind} ", " allowed_text)
            list(APPEND violations
                "${module} (${kind}) depends on ${dependency} (${kind_${dependency}}), but ${kind} modules may depend only on ${allowed_text}")
        endif()
    endforeach()
endforeach()

# The modules reachable from `roots` through their dependencies. A root is in the result only
# when a cycle leads back to it.
function(resolve_closure roots out_var)
    set(closure "")
    set(pending "")
    foreach(root IN LISTS roots)
        list(APPEND pending ${direct_${root}})
    endforeach()
    while(pending)
        list(POP_FRONT pending current)
        if(NOT current IN_LIST closure)
            list(APPEND closure ${current})
            list(APPEND pending ${direct_${current}})
        endif()
    endwhile()
    set(${out_var} "${closure}" PARENT_SCOPE)
endfunction()

# Adds a violation for each "ith/<module>/" include in the source files after `allowed` (absolute
# paths) that names a module outside `allowed`. `owner` names the module or application in the
# message. Includes of an unavailable module (UNAVAILABLE_MODULES: one that exists only on other
# platforms, like rhi_d3d12 outside Windows) are allowed: they sit behind a platform check, and the
# builds that declare the module check them.
function(check_includes owner allowed)
    foreach(source IN LISTS ARGN)
        file(STRINGS "${source}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*\"ith/")
        foreach(line IN LISTS includes)
            if(line MATCHES "\"ith/([a-z0-9_]+)/")
                set(included "${CMAKE_MATCH_1}")
                if(NOT included IN_LIST allowed AND NOT included IN_LIST unavailable_modules)
                    file(RELATIVE_PATH relative "${REPOSITORY_DIR}" "${source}")
                    list(APPEND violations "${relative} includes module '${included}', which ${owner} does not depend on")
                endif()
            endif()
        endforeach()
    endforeach()
    set(violations "${violations}" PARENT_SCOPE)
endfunction()

# Rule 2: includes stay inside the dependency closure.
foreach(module IN LISTS modules)
    resolve_closure(${module} allowed)
    if(module IN_LIST allowed)
        list(APPEND violations "${module} depends on itself through a cycle")
    endif()
    list(APPEND allowed ${module})

    file(GLOB_RECURSE sources "${ENGINE_DIR}/${module}/*.h" "${ENGINE_DIR}/${module}/*.cpp" "${ENGINE_DIR}/${module}/*.inl")
    check_includes(${module} "${allowed}" ${sources})

    if(NOT EXISTS "${ENGINE_DIR}/${module}")
        list(APPEND violations "module '${module}' has no directory engine/ith/${module}")
    endif()
endforeach()

# Rule 5: the kinds and includes of each application.
set(applications "")
foreach(entry IN LISTS application_entries)
    if(NOT entry MATCHES "^([a-z0-9_]+):([a-z]+):([^=]*)=(.*)$")
        message(FATAL_ERROR "Malformed application table entry: '${entry}'")
    endif()
    set(application "${CMAKE_MATCH_1}")
    set(target "${CMAKE_MATCH_2}")
    string(REPLACE "," ";" relative_sources "${CMAKE_MATCH_3}")
    string(REPLACE "," ";" linked "${CMAKE_MATCH_4}")
    list(APPEND applications ${application})

    if(NOT DEFINED target_kinds_${target})
        list(APPEND violations "application ${application} has unknown target '${target}'")
        continue()
    endif()

    set(known "")
    foreach(module IN LISTS linked)
        if(module IN_LIST modules)
            list(APPEND known ${module})
        else()
            list(APPEND violations "application ${application} links unknown module '${module}'")
        endif()
    endforeach()

    resolve_closure("${known}" closure)
    list(APPEND closure ${known})
    list(REMOVE_DUPLICATES closure)
    foreach(module IN LISTS closure)
        if(NOT kind_${module} IN_LIST target_kinds_${target})
            list(JOIN target_kinds_${target} ", " allowed_text)
            list(APPEND violations
                "application ${application} (${target}) links ${module} (${kind_${module}}), but ${target} applications may link only ${allowed_text}")
        endif()
    endforeach()

    set(sources "")
    foreach(source IN LISTS relative_sources)
        list(APPEND sources "${REPOSITORY_DIR}/${source}")
    endforeach()
    check_includes("application ${application}" "${closure}" ${sources})
endforeach()

if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "Module layering violations:\n  ${text}")
endif()
list(LENGTH modules module_count)
list(LENGTH applications application_count)
message(STATUS "Module layering: ${module_count} module(s) and ${application_count} application(s) clean.")
