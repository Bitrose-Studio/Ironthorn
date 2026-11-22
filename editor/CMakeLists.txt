# The editor application (ADR-0008, ADR-0029): the editor shell over one editor session. It compiles
# its UI shader at start-up with the shader compiler, which sanitizer builds leave out, so it is
# built only with ITH_BUILD_SHADER_COMPILER; its modules are built and tested headless without it.
if(NOT (TARGET ith_editor_ui AND ITH_BUILD_SHADER_COMPILER))
    return()
endif()

ith_add_application(ironthorn_editor
    KIND editor
    SOURCES Main.cpp
    MODULES
        foundation
        file_system
        jobs
        platform
        input
        rhi
        rhi_select
        render_graph
        shader_compiler
        reflection
        config
        ecs
        editor_core
        editor_ui)
ith_copy_shader_compiler_runtime(ironthorn_editor)
add_dependencies(ironthorn_editor ith_engine_config ith_editor_fonts)

# The editor started by CTest on SDL's offscreen driver, never on a desktop (ADR-0029 rule 18): it
# draws the shell, runs tests/editor/open_edit_save_reopen.json in a fresh project and exits 0.
# Once on the null device, and once on the software GPU (WARP, lavapipe; label gpu), which
# compiles and draws the UI shader for real; without a Vulkan driver the latter falls back to the
# null device. Not in Release: editor applications aren't built there.
if(IRONTHORN_BUILD_TESTS)
    foreach(backend null gpu)
        set(directory "${CMAKE_CURRENT_BINARY_DIR}/automation_${backend}")
        if(backend STREQUAL "null")
            set(arguments --backend null)
            set(labels "sample")
        else()
            set(arguments --adapter software)
            set(labels "sample;gpu")
        endif()
        # Tests of their own clear the folder and create an empty project; the run depends on them.
        add_test(NAME editor_automation_${backend}_clean
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${directory}"
            CONFIGURATIONS Debug RelWithDebInfo)
        add_test(NAME editor_automation_${backend}_project
            COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}/Project/Content"
            CONFIGURATIONS Debug RelWithDebInfo)
        set_tests_properties(editor_automation_${backend}_clean editor_automation_${backend}_project PROPERTIES
            LABELS "${labels}"
            FIXTURES_SETUP editor_project_${backend})
        set_tests_properties(editor_automation_${backend}_project PROPERTIES DEPENDS editor_automation_${backend}_clean)
        add_test(NAME editor_automation_${backend}
            COMMAND ironthorn_editor --project "${directory}/Project" --user-dir "${directory}/User"
                --automation "${PROJECT_SOURCE_DIR}/tests/editor/open_edit_save_reopen.json" ${arguments}
            CONFIGURATIONS Debug RelWithDebInfo)
        set_tests_properties(editor_automation_${backend} PROPERTIES
            LABELS "${labels}"
            FIXTURES_REQUIRED editor_project_${backend}
            ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
            TIMEOUT 300)
    endforeach()
endif()
