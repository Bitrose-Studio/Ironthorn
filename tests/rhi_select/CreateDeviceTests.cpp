#include "ith/rhi_select/CreateDevice.h"

#include "ith/rhi/Capabilities.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"

#include <doctest/doctest.h>

using namespace ith::rhi;
using ith::Expected;

// Backend selection without a GPU: the Direct3D 12 path itself is covered by the rhi_d3d12 suite.
TEST_SUITE("rhi_select") {
    TEST_CASE("the default backend is Direct3D 12 on Windows and Vulkan elsewhere") {
#ifdef _WIN32
        CHECK(defaultBackend() == Backend::D3D12);
#else
        CHECK(defaultBackend() == Backend::Vulkan);
#endif
    }

    TEST_CASE("the null backend is created on request") {
        Expected<Device, Error> device = createDevice({}, Backend::Null);
        REQUIRE(device.hasValue());
        CHECK(device->capabilities().backend == Backend::Null);
    }

    TEST_CASE("Vulkan is built in: it creates a device, or names why it can't") {
        // Without a Vulkan driver (hosted CI runners) the backend reports that it is unavailable;
        // the device itself is covered by the rhi_vulkan suite.
        const Expected<Device, Error> device = createDevice({.validation = false}, Backend::Vulkan);
        if (device) {
            CHECK(device->capabilities().backend == Backend::Vulkan);
        } else {
            MESSAGE(device.error().toString());
            CHECK((device.error().code == ErrorCode::BackendUnavailable ||
                   device.error().code == ErrorCode::UnsupportedDevice));
        }
    }

#ifndef _WIN32
    TEST_CASE("Direct3D 12 is unavailable outside Windows") {
        const Expected<Device, Error> device = createDevice({}, Backend::D3D12);
        REQUIRE_FALSE(device.hasValue());
        CHECK(device.error().code == ErrorCode::BackendUnavailable);
    }
#endif
}
