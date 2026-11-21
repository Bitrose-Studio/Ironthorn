# Helpers for declaring engine modules and their test suites.
#
# An engine module is a static library under engine/ith/<name>, with a layer from the
# architecture (ADR-0004). A module may depend only on modules of the same or a lower layer,
# listed in DEPENDS, without cycles. The module_layering test fails when that rule is broken,
# or when a module includes a header of a module outside its dependency closure.

# Layers, lowest first. The number is the layer's rank.
set(ITH_LAYERS foundation platform core engine framework editor application)

# ith_add_module(<name>
#     LAYER <layer>
#     SOURCES <files...>
#     [DEPENDS <module names...>]
#     [PUBLIC_LIBRARIES <targets...>]
#     [PRIVATE_LIBRARIES <targets...>]
#     [REFLECT])
#
# REFLECT runs the reflection generator over the module's headers (the .h files in SOURCES)
# and compiles the result into the module; it adds a dependency on the reflection module.
function(ith_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "REFLECT" "LAYER" "SOURCES;DEPENDS;PUBLIC_LIBRARIES;PRIVATE_LIBRARIES")

    if(NOT arg_LAYER IN_LIST ITH_LAYERS)
        message(FATAL_ERROR "Module '${name}': LAYER must be one of: ${ITH_LAYERS}")
    endif()

    if(arg_REFLECT AND NOT "reflection" IN_LIST arg_DEPENDS)
        list(APPEND arg_DEPENDS reflection)
    endif()

    set(target ith_${name})
    add_library(${target} STATIC ${arg_SOURCES})
    add_library(ith::${name} ALIAS ${target})

    target_include_directories(${target} PUBLIC
        "${PROJECT_SOURCE_DIR}/engine"
        "${PROJECT_BINARY_DIR}/generated")

    set(module_targets "")
    foreach(dependency IN LISTS arg_DEPENDS)
        list(APPEND module_targets ith::${dependency})
    endforeach()

    target_link_libraries(${target}
        PUBLIC ith::build_options ${module_targets} ${arg_PUBLIC_LIBRARIES}
        PRIVATE ith::runtime_options ${arg_PRIVATE_LIBRARIES})

    set_target_properties(${target} PROPERTIES
        FOLDER "Ironthorn/Engine"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})

    set_property(GLOBAL APPEND PROPERTY ITH_MODULES ${name})
    set_property(GLOBAL PROPERTY ITH_MODULE_LAYER_${name} "${arg_LAYER}")
    set_property(GLOBAL PROPERTY ITH_MODULE_DEPENDS_${name} "${arg_DEPENDS}")

    if(arg_REFLECT)
        # Module kinds (ADR-0017) are not declared yet; editor-layer modules are the editor kind.
        set(kind runtime)
        if(arg_LAYER STREQUAL "editor")
            set(kind editor)
        endif()
        set(headers "")
        foreach(source IN LISTS arg_SOURCES)
            if(source MATCHES "\\.h$")
                list(APPEND headers "${source}")
            endif()
        endforeach()
        ith_reflect_target(${target}
            MODULE ${name}
            KIND ${kind}
            # Engine modules only for now: game modules will pass their own include root.
            INCLUDE_ROOT "${PROJECT_SOURCE_DIR}/engine"
            HEADERS ${headers})
    endif()
endfunction()

# ith_reflect_target(<target>
#     MODULE <name>
#     KIND <runtime|client|tools|editor>
#     INCLUDE_ROOT <dir>
#     HEADERS <headers...>)
#
# Generates <name>.reflect.cpp from the annotated HEADERS with tools/ith_reflect and adds it to
# <target> (ADR-0018). The generated file includes each header by its path relative to
# INCLUDE_ROOT and defines ith::generated::registerReflection_<name>. The generator reruns only
# when one of the headers or the generator itself changes, and rewrites the file only when its
# content changes. Each configuration gets its own copy, built by that configuration's generator.
# ith_add_module(... REFLECT) calls this; tests use it directly for test-only reflected types.
function(ith_reflect_target target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "MODULE;KIND;INCLUDE_ROOT" "HEADERS")

    set(headers "")
    foreach(header IN LISTS arg_HEADERS)
        cmake_path(ABSOLUTE_PATH header BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE absolute)
        list(APPEND headers "${absolute}")
    endforeach()

    set(output "${PROJECT_BINARY_DIR}/generated/${arg_MODULE}/$<CONFIG>/${arg_MODULE}.reflect.cpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND ith_reflect
            --module ${arg_MODULE}
            --kind ${arg_KIND}
            --include-root "${arg_INCLUDE_ROOT}"
            --output "${output}"
            ${headers}
        DEPENDS ith_reflect ${headers}
        COMMENT "Generating reflection for ${arg_MODULE}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()

# ith_add_test_suite(<name> SOURCES <files...> LIBRARIES <targets...> [LABELS <labels...>] [TIMEOUT <s>])
#
# Creates the executable ith_<name>_tests and registers it with CTest as <name>. Suites that
# need a GPU carry the 'gpu' label, so headless machines can skip them with: ctest -LE gpu
function(ith_add_test_suite name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TIMEOUT" "SOURCES;LIBRARIES;LABELS")

    set(target ith_${name}_tests)
    add_executable(${target} ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE ith::test_support ${arg_LIBRARIES})
    set_target_properties(${target} PROPERTIES
        FOLDER "Ironthorn/Tests"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})

    if(NOT arg_TIMEOUT)
        set(arg_TIMEOUT 300)
    endif()
    add_test(NAME ${name} COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(${name} PROPERTIES LABELS "unit;${arg_LABELS}" TIMEOUT ${arg_TIMEOUT})
endfunction()

# Registers the module_layering test. Call after every module has been declared.
function(ith_add_layering_test)
    get_property(modules GLOBAL PROPERTY ITH_MODULES)

    set(table "")
    foreach(module IN LISTS modules)
        get_property(layer GLOBAL PROPERTY ITH_MODULE_LAYER_${module})
        get_property(dependencies GLOBAL PROPERTY ITH_MODULE_DEPENDS_${module})
        string(REPLACE ";" "," dependencies "${dependencies}")
        list(APPEND table "${module}:${layer}=${dependencies}")
    endforeach()
    string(REPLACE ";" "|" table "${table}")
    string(REPLACE ";" "," layers "${ITH_LAYERS}")

    add_test(NAME module_layering
        COMMAND ${CMAKE_COMMAND}
            "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            "-DMODULE_TABLE=${table}"
            "-DLAYERS=${layers}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckModuleLayering.cmake")
    set_tests_properties(module_layering PROPERTIES LABELS "lint")
endfunction()
