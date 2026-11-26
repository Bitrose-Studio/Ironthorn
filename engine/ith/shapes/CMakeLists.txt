# Primitive shapes for the editor's preview (ADR-0042 rule 19): the reflected ShapeRenderer
# component, saved into levels like any component, and each shape's local bounds and unit mesh.
# Data only: no GPU code. Mesh components replace it in P3.
ith_add_module(shapes
    LAYER core
    SOURCES
        ShapeMesh.h
        ShapeMesh.cpp
        ShapeRenderer.h
    DEPENDS
        foundation
        math
    REFLECT)
