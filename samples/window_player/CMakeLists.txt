# A player that opens a window and runs the platform event loop (ADR-0012).
ith_add_application(window_player
    KIND player
    SOURCES Main.cpp
    MODULES foundation platform)

# Runs headless on SDL's offscreen video driver, so it needs no display.
if(IRONTHORN_BUILD_TESTS AND TARGET window_player)
    add_test(NAME sample_window_player COMMAND window_player --frames 10)
    set_tests_properties(sample_window_player PROPERTIES
        LABELS "sample"
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
endif()
