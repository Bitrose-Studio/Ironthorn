# Enforces the module layering rules (ADR-0004):
#   1. A module depends only on modules of the same or a lower layer, and dependencies have no cycles.
#   2. A module includes only its own headers and those of modules in its dependency closure.
#
# Usage:
#   cmake -DREPOSITORY_DIR=<repo>
#         -DMODULE_TABLE="foundation:foundation=|platform:platform=foundation"
#         -DLAYERS="foundation,platform,core,..."
#         -P CheckModuleLayering.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR OR NOT DEFINED MODULE_TABLE OR NOT DEFINED LAYERS)
    message(FATAL_ERROR "REPOSITORY_DIR, MODULE_TABLE and LAYERS must be defined.")
endif()
set(ENGINE_DIR "${REPOSITORY_DIR}/engine/ith")

string(REPLACE "," ";" layers "${LAYERS}")
string(REPLACE "|" ";" entries "${MODULE_TABLE}")

set(modules "")
foreach(entry IN LISTS entries)
    if(NOT entry MATCHES "^([a-z0-9_]+):([a-z]+)=(.*)$")
        message(FATAL_ERROR "Malformed module table entry: '${entry}'")
    endif()
    set(module "${CMAKE_MATCH_1}")
    list(APPEND modules ${module})
    list(FIND layers "${CMAKE_MATCH_2}" rank_${module})
    string(REPLACE "," ";" direct_${module} "${CMAKE_MATCH_3}")
endforeach()

set(violations "")

# Rule 1: dependencies never point upwards.
foreach(module IN LISTS modules)
    foreach(dependency IN LISTS direct_${module})
        if(NOT dependency IN_LIST modules)
            list(APPEND violations "${module} depends on unknown module '${dependency}'")
        elseif(rank_${dependency} GREATER rank_${module})
            list(APPEND violations
                "${module} (layer ${rank_${module}}) depends on ${dependency} (layer ${rank_${dependency}}), a higher layer")
        endif()
    endforeach()
endforeach()

# Rule 2: includes stay inside the dependency closure.
function(resolve_closure module out_var)
    set(closure "")
    set(pending ${direct_${module}})
    while(pending)
        list(POP_FRONT pending current)
        if(NOT current IN_LIST closure)
            list(APPEND closure ${current})
            list(APPEND pending ${direct_${current}})
        endif()
    endwhile()
    set(${out_var} "${closure}" PARENT_SCOPE)
endfunction()

foreach(module IN LISTS modules)
    resolve_closure(${module} allowed)
    if(module IN_LIST allowed)
        list(APPEND violations "${module} depends on itself through a cycle")
    endif()
    list(APPEND allowed ${module})

    file(GLOB_RECURSE sources "${ENGINE_DIR}/${module}/*.h" "${ENGINE_DIR}/${module}/*.cpp" "${ENGINE_DIR}/${module}/*.inl")
    foreach(source IN LISTS sources)
        file(STRINGS "${source}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*\"ith/")
        foreach(line IN LISTS includes)
            if(line MATCHES "\"ith/([a-z0-9_]+)/")
                set(included "${CMAKE_MATCH_1}")
                if(NOT included IN_LIST allowed)
                    file(RELATIVE_PATH relative "${REPOSITORY_DIR}" "${source}")
                    list(APPEND violations "${relative} includes module '${included}', which ${module} does not depend on")
                endif()
            endif()
        endforeach()
    endforeach()

    if(NOT EXISTS "${ENGINE_DIR}/${module}")
        list(APPEND violations "module '${module}' has no directory engine/ith/${module}")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "Module layering violations:\n  ${text}")
endif()
list(LENGTH modules count)
message(STATUS "Module layering: ${count} module(s) clean.")
