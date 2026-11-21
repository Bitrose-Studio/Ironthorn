configure_file(BuildInfo.h.in "${PROJECT_BINARY_DIR}/generated/ith/foundation/BuildInfo.h" @ONLY)

ith_add_module(foundation
    LAYER foundation
    SOURCES
        Assert.h
        Assert.cpp
        Compiler.h
        Expected.h
        FixedTimestep.h
        FixedTimestep.cpp
        Hash.h
        LinearAllocator.h
        LinearAllocator.cpp
        Mutex.h
        ObjectPool.h
        Stopwatch.h
        StringUtils.h
        StringUtils.cpp
        Uuid.h
        Uuid.cpp
        Version.h
        BuildInfo.h.in)
