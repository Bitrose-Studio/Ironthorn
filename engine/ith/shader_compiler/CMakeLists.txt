# Compiles Slang shaders to SPIR-V and DXIL (ADR-0011). Tools only: the editor and the cooker
# link it, a game never does, which the editor layer guarantees (ADR-0004).
ith_add_module(shader_compiler
    LAYER editor
    SOURCES
        ShaderCompiler.h
        ShaderCompiler.cpp
    DEPENDS
        foundation
    PRIVATE_LIBRARIES
        ith::slang)
