// The RHI's format queries: texel sizes as copies lay them out, depth formats, integer formats,
// and which formats every backend can sample with a linear filter.

#include "ith/rhi/Format.h"

#include <doctest/doctest.h>

#include <array>

using namespace ith::rhi;

TEST_SUITE("rhi.format") {
    TEST_CASE("texel sizes are the bytes one texel takes in copied buffer data") {
        CHECK(formatTexelSize(Format::Unknown) == 0);
        CHECK(formatTexelSize(Format::R8Unorm) == 1);
        CHECK(formatTexelSize(Format::R8G8Unorm) == 2);
        CHECK(formatTexelSize(Format::R8G8B8A8Unorm) == 4);
        CHECK(formatTexelSize(Format::R8G8B8A8Srgb) == 4);
        CHECK(formatTexelSize(Format::B8G8R8A8Unorm) == 4);
        CHECK(formatTexelSize(Format::B8G8R8A8Srgb) == 4);
        CHECK(formatTexelSize(Format::R10G10B10A2Unorm) == 4);
        CHECK(formatTexelSize(Format::R11G11B10Float) == 4);
        CHECK(formatTexelSize(Format::R16G16Float) == 4);
        CHECK(formatTexelSize(Format::R16G16B16A16Float) == 8);
        CHECK(formatTexelSize(Format::R32Uint) == 4);
        CHECK(formatTexelSize(Format::R32G32Uint) == 8);
        CHECK(formatTexelSize(Format::R32Float) == 4);
        CHECK(formatTexelSize(Format::R32G32Float) == 8);
        CHECK(formatTexelSize(Format::R32G32B32A32Float) == 16);
        CHECK(formatTexelSize(Format::D32Float) == 4);
        // Copies address the depth plane only.
        CHECK(formatTexelSize(Format::D32FloatS8Uint) == 4);
    }

    TEST_CASE("only the two depth formats are depth formats") {
        CHECK(isDepthFormat(Format::D32Float));
        CHECK(isDepthFormat(Format::D32FloatS8Uint));
        CHECK_FALSE(isDepthFormat(Format::Unknown));
        CHECK_FALSE(isDepthFormat(Format::R32Float));
        CHECK_FALSE(isDepthFormat(Format::R32G32Uint));
    }

    TEST_CASE("only R32Uint and R32G32Uint are integer formats") {
        CHECK(isIntegerFormat(Format::R32Uint));
        CHECK(isIntegerFormat(Format::R32G32Uint));
        constexpr std::array kOthers{Format::Unknown,       Format::R8Unorm,           Format::R8G8B8A8Unorm,
                                     Format::R8G8B8A8Srgb,  Format::R16G16Float,       Format::R32Float,
                                     Format::R32G32Float,   Format::R32G32B32A32Float, Format::D32Float,
                                     Format::D32FloatS8Uint};
        for (const Format format : kOthers) {
            CAPTURE(static_cast<int>(format));
            CHECK_FALSE(isIntegerFormat(format));
        }
    }

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
        CHECK_FALSE(isLinearFilterable(Format::R32G32Uint));
        CHECK_FALSE(isLinearFilterable(Format::R32Float));
        CHECK_FALSE(isLinearFilterable(Format::R32G32Float));
        CHECK_FALSE(isLinearFilterable(Format::R32G32B32A32Float));
        CHECK_FALSE(isLinearFilterable(Format::D32Float));
        CHECK_FALSE(isLinearFilterable(Format::D32FloatS8Uint));
    }
}
