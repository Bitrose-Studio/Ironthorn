# Sample applications (ADR-0017). They are kept minimal: they exist so the target check of the
# module_layering test and the shipping link test in CI have a player and a server to check.
# Each runs once as a CTest test.

add_subdirectory(minimal_player)
add_subdirectory(minimal_server)
