// A lint fixture, never compiled: a source outside engine/ith/math that includes glm directly.
// The glm_includes_refuses_direct_include test expects the check to report both includes; the
// glm_includes test skips this directory.

#include "glm/glm.hpp"

#include <glm/vec3.hpp>
