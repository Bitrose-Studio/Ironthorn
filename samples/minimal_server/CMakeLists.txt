# The smallest dedicated server: links only runtime modules.
ith_add_application(minimal_server
    KIND server
    SOURCES Main.cpp
    MODULES foundation ecs)

if(IRONTHORN_BUILD_TESTS)
    add_test(NAME sample_minimal_server COMMAND minimal_server)
    set_tests_properties(sample_minimal_server PROPERTIES LABELS "sample")
endif()
