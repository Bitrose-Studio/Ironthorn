# A player that opens a window, runs the platform event loop (ADR-0012) and clears the window
# through the RHI where it can render (ADR-0009).
ith_add_application(window_player
    KIND player
    SOURCES Main.cpp
    MODULES foundation jobs platform rhi rhi_select)

# Runs headless on SDL's offscreen video driver, so it needs no display.
if(IRONTHORN_BUILD_TESTS AND TARGET window_player)
    add_test(NAME sample_window_player COMMAND window_player --frames 10)
    set_tests_properties(sample_window_player PROPERTIES
        LABELS "sample"
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
endif()
