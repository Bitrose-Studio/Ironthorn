# The runtime home of game worlds (ADR-0053 decision 1): the Time world resource and its advance,
# and the game-world schedule setup and tick that the player, the dedicated server and Play In
# Editor share. Later also the lifecycle events and the game-world system list.
ith_add_module(gameplay
    LAYER framework
    KIND runtime
    SOURCES
        GameWorld.h
        GameWorld.cpp
        Time.h
    DEPENDS
        foundation
        reflection
        ecs
    REFLECT)
