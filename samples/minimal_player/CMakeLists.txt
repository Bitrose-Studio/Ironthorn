# The smallest player: links only runtime and client modules.
ith_add_application(minimal_player
    KIND player
    SOURCES Main.cpp
    MODULES foundation ecs)

if(IRONTHORN_BUILD_TESTS AND TARGET minimal_player)
    add_test(NAME sample_minimal_player COMMAND minimal_player)
    set_tests_properties(sample_minimal_player PROPERTIES LABELS "sample")
endif()
