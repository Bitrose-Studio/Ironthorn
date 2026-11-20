configure_file(BuildInfo.h.in "${PROJECT_BINARY_DIR}/generated/ith/foundation/BuildInfo.h" @ONLY)

ith_add_module(foundation
    LAYER foundation
    SOURCES
        Assert.h
        Assert.cpp
        Expected.h
        Version.h
        BuildInfo.h.in)
