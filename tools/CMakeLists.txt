# Build tools that run on the host during the engine build. They are not engine modules and are
# linked into nothing; they are built in every configuration, with or without
# IRONTHORN_BUILD_TOOLS, because the engine build itself needs them.
#
# They are built with the target toolchain, which is the host toolchain on every platform today.
# Cross-compiling (consoles) will need them built for the host separately.

add_subdirectory(ith_reflect)
