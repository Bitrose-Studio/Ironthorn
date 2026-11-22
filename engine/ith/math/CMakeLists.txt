# Math (ADR-0006): the engine's names for glm's vectors, matrices and quaternions, glm's functions
# in the engine's configuration, the reversed infinite projection, Transform, bounds, frustums and
# ray tests. glm is public here and only here: ith::glm carries its configuration defines, and
# the glm_includes test refuses a direct glm include outside this directory.
ith_add_module(math
    LAYER foundation
    SOURCES
        Geometry.h
        Geometry.cpp
        Math.h
        Math.cpp
        Transform.h
        Transform.cpp
        Types.h
    DEPENDS
        foundation
    PUBLIC_LIBRARIES
        ith::glm)
