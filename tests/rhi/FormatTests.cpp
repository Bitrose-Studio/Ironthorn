// The RHI's format queries: which formats every backend can sample with a linear filter.

#include "ith/rhi/Format.h"

#include <doctest/doctest.h>

using namespace ith::rhi;

TEST_SUITE("rhi.format") {
    TEST_CASE("normalized, sRGB, packed and 16-bit float colour formats are linear filterable") {
        CHECK(isLinearFilterable(Format::R8Unorm));
        CHECK(isLinearFilterable(Format::R8G8Unorm));
        CHECK(isLinearFilterable(Format::R8G8B8A8Unorm));
        CHECK(isLinearFilterable(Format::R8G8B8A8Srgb));
        CHECK(isLinearFilterable(Format::B8G8R8A8Unorm));
        CHECK(isLinearFilterable(Format::B8G8R8A8Srgb));
        CHECK(isLinearFilterable(Format::R10G10B10A2Unorm));
        CHECK(isLinearFilterable(Format::R11G11B10Float));
        CHECK(isLinearFilterable(Format::R16G16Float));
        CHECK(isLinearFilterable(Format::R16G16B16A16Float));
    }

    TEST_CASE("unknown, integer, 32-bit float and depth formats are not linear filterable") {
        CHECK_FALSE(isLinearFilterable(Format::Unknown));
        CHECK_FALSE(isLinearFilterable(Format::R32Uint));
        CHECK_FALSE(isLinearFilterable(Format::R32Float));
        CHECK_FALSE(isLinearFilterable(Format::R32G32Float));
        CHECK_FALSE(isLinearFilterable(Format::R32G32B32A32Float));
        CHECK_FALSE(isLinearFilterable(Format::D32Float));
        CHECK_FALSE(isLinearFilterable(Format::D32FloatS8Uint));
    }
}
