configure_file(BuildInfo.h.in "${PROJECT_BINARY_DIR}/generated/ith/foundation/BuildInfo.h" @ONLY)

ith_add_module(foundation
    LAYER foundation
    SOURCES
        Assert.h
        Assert.cpp
        Compiler.h
        Entity.h
        Expected.h
        FixedTimestep.h
        FixedTimestep.cpp
        Hash.h
        LinearAllocator.h
        LinearAllocator.cpp
        Log.h
        Log.cpp
        LogRingBuffer.h
        LogRingBuffer.cpp
        Mutex.h
        ObjectPool.h
        Profile.h
        Stopwatch.h
        StringUtils.h
        StringUtils.cpp
        Uuid.h
        Uuid.cpp
        Version.h
        BuildInfo.h.in)

# Profiling builds (ADR-0007 rule 13): Profile.h's macros use the Tracy client. Both are public, so
# every target that can include Profile.h sees the same configuration.
if(IRONTHORN_PROFILING)
    target_link_libraries(ith_foundation PUBLIC ith::tracy)
    target_compile_definitions(ith_foundation PUBLIC ITH_ENABLE_PROFILING=1)
endif()
