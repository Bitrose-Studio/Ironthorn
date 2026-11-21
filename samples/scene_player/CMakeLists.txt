# A player that draws through the RHI's bindless model (ADR-0009): a triangle, or 10,000
# instanced cubes with a depth buffer in one draw, and prints its frame timings. Its shaders are
# compiled at build time by the pinned slangc (ith_embed_shaders, ADR-0023), which comes with the
# shader compiler, so it is built only with ITH_BUILD_SHADER_COMPILER (the tools, without
# sanitizers); a game loads cooked shaders instead (ADR-0011).
if(NOT (IRONTHORN_BUILD_CLIENT AND ITH_BUILD_SHADER_COMPILER))
    return()
endif()

# Embeds the scenes' shaders in <target>, which compiles Scenes.cpp: the sample, and its GPU test
# (tests/scene_player). DXIL only where the Direct3D 12 backend is built.
function(ith_scene_player_shaders target)
    set(dxil "")
    if(TARGET ith_rhi_d3d12)
        set(dxil DXIL)
        target_compile_definitions(${target} PRIVATE ITH_SCENE_PLAYER_HAS_DXIL=1)
    endif()
    set(directory "${PROJECT_SOURCE_DIR}/samples/scene_player")
    ith_embed_shaders(${target} SOURCE "${directory}/Triangle.slang"
        ENTRY_POINTS triangleVertex:vertex trianglePixel:pixel ${dxil})
    ith_embed_shaders(${target} SOURCE "${directory}/Cubes.slang"
        ENTRY_POINTS cubeVertex:vertex cubePixel:pixel ${dxil})
endfunction()

# Scenes.cpp is one of the application's sources, so the module_layering test checks its includes.
ith_add_application(scene_player
    KIND player
    SOURCES Main.cpp Scenes.h Scenes.cpp
    MODULES foundation platform rhi rhi_select)
ith_scene_player_shaders(scene_player)

# Runs headless on SDL's offscreen video driver, without rendering; the GPU test draws the scenes.
if(IRONTHORN_BUILD_TESTS)
    add_test(NAME sample_scene_player COMMAND scene_player --frames 10)
    set_tests_properties(sample_scene_player PROPERTIES
        LABELS "sample"
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
endif()
