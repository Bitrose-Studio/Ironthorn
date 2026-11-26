// A lint fixture for the tracy_confined check (ADR-0007): Tracy included outside
// ith/foundation/Profile.h and the RHI backends' GPU-zone sources. It is never compiled.

#include "TracyVulkan.hpp"

#include <TracyC.h>
#include <tracy/Tracy.hpp>
