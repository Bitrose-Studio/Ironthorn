# Helpers for declaring engine modules, applications and test suites.
#
# An engine module is a static library under engine/ith/<name>, with a layer from the
# architecture (ADR-0004) and a kind (ADR-0017). A module may depend only on modules of the same
# or a lower layer and of an allowed kind, listed in DEPENDS, without cycles. An application is
# an executable of one target (editor, player, server or tools) that may link only the kinds its
# target allows. The module_layering test fails when one of these rules is broken, or when a
# module or application includes a header of a module outside its dependency closure.

# Layers, lowest first. The number is the layer's rank.
set(ITH_LAYERS foundation platform core engine framework editor application)

# Module kinds (ADR-0017): runtime (every game process), client (not on a dedicated server),
# tools (offline processing) and editor. IRONTHORN_BUILD_CLIENT, _TOOLS and _EDITOR turn the
# last three off; a module of a kind that is off is not declared at all.
set(ITH_MODULE_KINDS runtime client tools editor)

# Application targets (ADR-0017).
set(ITH_APPLICATION_TARGETS editor player server tools)

# ith_add_module(<name>
#     LAYER <layer>
#     [KIND <runtime|client|tools|editor>]
#     SOURCES <files...>
#     [DEPENDS <module names...>]
#     [PUBLIC_LIBRARIES <targets...>]
#     [PRIVATE_LIBRARIES <targets...>]
#     [REFLECT])
#
# KIND defaults to runtime and is stored, like LAYER and DEPENDS, in a global property
# (ITH_MODULE_KIND_<name>) that the module_layering test reads. When the kind is
# turned off (IRONTHORN_BUILD_CLIENT, IRONTHORN_BUILD_TOOLS or IRONTHORN_BUILD_EDITOR), the
# module is skipped: no target exists, so anything that links it fails to configure. runtime and
# client modules belong in the layers foundation to framework, tools and editor modules in the
# editor layer; the module_layering test checks that.
#
# REFLECT runs the reflection generator over the module's headers (the .h files in SOURCES)
# and compiles the result into the module, telling the generator the module's KIND; it adds a
# dependency on the reflection module.
function(ith_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "REFLECT" "LAYER;KIND" "SOURCES;DEPENDS;PUBLIC_LIBRARIES;PRIVATE_LIBRARIES")

    if(NOT arg_LAYER IN_LIST ITH_LAYERS)
        message(FATAL_ERROR "Module '${name}': LAYER must be one of: ${ITH_LAYERS}")
    endif()
    if(NOT arg_KIND)
        set(arg_KIND runtime)
    endif()
    if(NOT arg_KIND IN_LIST ITH_MODULE_KINDS)
        message(FATAL_ERROR "Module '${name}': KIND must be one of: ${ITH_MODULE_KINDS}")
    endif()
    if((arg_KIND STREQUAL "client" AND NOT IRONTHORN_BUILD_CLIENT)
            OR (arg_KIND STREQUAL "tools" AND NOT IRONTHORN_BUILD_TOOLS)
            OR (arg_KIND STREQUAL "editor" AND NOT IRONTHORN_BUILD_EDITOR))
        return()
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
    set_property(GLOBAL PROPERTY ITH_MODULE_KIND_${name} "${arg_KIND}")

    if(arg_REFLECT)
        set(headers "")
        foreach(source IN LISTS arg_SOURCES)
            if(source MATCHES "\\.h$")
                list(APPEND headers "${source}")
            endif()
        endforeach()
        ith_reflect_target(${target}
            MODULE ${name}
            KIND ${arg_KIND}
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

# ith_add_application(<name>
#     KIND <editor|player|server|tools>
#     SOURCES <files...>
#     MODULES <module names...>)
#
# Declares the executable <name>, an application of one target (ADR-0017), linking MODULES. The
# module_layering test checks that the full dependency closure of MODULES has only the kinds the
# target allows: editor any; player runtime and client; server runtime; tools runtime and tools.
# An application is skipped when its target's modules are turned off: the editor with
# IRONTHORN_BUILD_EDITOR, the player with IRONTHORN_BUILD_CLIENT, tools with
# IRONTHORN_BUILD_TOOLS. Editor and tools applications are never built in Shipping: they are
# left out of the default build of the Release configuration (the "all" target of Ninja
# Multi-Config, the solution or project build of Visual Studio and Xcode). A test that runs an
# editor or tools application must therefore not run in Release, e.g. by registering it with
# add_test(... CONFIGURATIONS Debug RelWithDebInfo).
function(ith_add_application name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "KIND" "SOURCES;MODULES")

    if(NOT arg_KIND IN_LIST ITH_APPLICATION_TARGETS)
        message(FATAL_ERROR "Application '${name}': KIND must be one of: ${ITH_APPLICATION_TARGETS}")
    endif()
    if((arg_KIND STREQUAL "editor" AND NOT IRONTHORN_BUILD_EDITOR)
            OR (arg_KIND STREQUAL "player" AND NOT IRONTHORN_BUILD_CLIENT)
            OR (arg_KIND STREQUAL "tools" AND NOT IRONTHORN_BUILD_TOOLS))
        return()
    endif()

    set(module_targets "")
    foreach(module IN LISTS arg_MODULES)
        list(APPEND module_targets ith::${module})
    endforeach()

    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE ith::build_options ith::runtime_options ${module_targets})
    set_target_properties(${name} PROPERTIES
        FOLDER "Ironthorn/Applications"
        COMPILE_WARNING_AS_ERROR ${IRONTHORN_WARNINGS_AS_ERRORS})
    if(arg_KIND STREQUAL "editor" OR arg_KIND STREQUAL "tools")
        # Only Ninja Multi-Config accepts a per-configuration EXCLUDE_FROM_ALL; Visual Studio and
        # Xcode have their own per-configuration property.
        if(CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
            set_target_properties(${name} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD_RELEASE ON)
        else()
            set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL "$<CONFIG:Release>")
        endif()
    endif()

    # The sources, relative to the repository, for the include check of the module_layering test.
    set(sources "")
    foreach(source IN LISTS arg_SOURCES)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE absolute)
        file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${absolute}")
        list(APPEND sources "${relative}")
    endforeach()
    string(REPLACE ";" "," sources "${sources}")
    string(REPLACE ";" "," modules "${arg_MODULES}")
    set_property(GLOBAL APPEND PROPERTY ITH_APPLICATIONS "${name}:${arg_KIND}:${sources}=${modules}")
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

# Registers the module_layering test. Call after every module and application has been declared.
function(ith_add_layering_test)
    get_property(modules GLOBAL PROPERTY ITH_MODULES)
    get_property(applications GLOBAL PROPERTY ITH_APPLICATIONS)

    set(table "")
    foreach(module IN LISTS modules)
        get_property(layer GLOBAL PROPERTY ITH_MODULE_LAYER_${module})
        get_property(dependencies GLOBAL PROPERTY ITH_MODULE_DEPENDS_${module})
        get_property(kind GLOBAL PROPERTY ITH_MODULE_KIND_${module})
        string(REPLACE ";" "," dependencies "${dependencies}")
        list(APPEND table "${module}:${layer}:${kind}=${dependencies}")
    endforeach()
    string(REPLACE ";" "|" table "${table}")
    string(REPLACE ";" "|" applications "${applications}")
    string(REPLACE ";" "," layers "${ITH_LAYERS}")

    add_test(NAME module_layering
        COMMAND ${CMAKE_COMMAND}
            "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
            "-DMODULE_TABLE=${table}"
            "-DAPPLICATION_TABLE=${applications}"
            "-DLAYERS=${layers}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckModuleLayering.cmake")
    set_tests_properties(module_layering PROPERTIES LABELS "lint")
endfunction()
