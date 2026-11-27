#include "ith/rhi/Device.h"
#include "ith/rhi/NullDevice.h"

#include "AssertCapture.h"
#include "RhiTestSupport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through it
#include <set>
#include <string>

using namespace ith::rhi;
using ith::Expected;
using ith::rhi::test::beginFrame;
using ith::rhi::test::emptyFrame;
using ith::rhi::test::endFrame;
using ith::rhi::test::makeBuffer;
using ith::rhi::test::makeDevice;

TEST_SUITE("rhi.frames") {
    TEST_CASE("frames alternate between kFramesInFlight slots and count up") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        for (std::uint64_t number = 0; number < 5; ++number) {
            const Frame frame = beginFrame(device);
            CHECK(frame.number == number);
            CHECK(frame.slot == number % kFramesInFlight);
            CHECK(frame.backbuffer.isNull());
            // Inside a frame, frameNumber() is that frame's number; it moves on at endFrame().
            CHECK(device.frameNumber() == frame.number);
            endFrame(device);
        }
        CHECK(device.frameNumber() == 5);
    }

    TEST_CASE("the CPU runs at most kFramesInFlight frames ahead of the GPU") {
        NullGpu gpu;
        gpu.setAutoComplete(false);
        Device device = makeDevice(gpu);

        emptyFrame(device); // frame 0
        const std::uint64_t frame0Value = gpu.signaledValue();
        emptyFrame(device); // frame 1: no wait, frame 0 may still run
        CHECK(gpu.waitCount() == 0);
        CHECK(gpu.completedValue() == 0);

        beginFrame(device); // frame 2 reuses frame 0's slot: it must wait for frame 0
        CHECK(gpu.waitCount() == 1);
        CHECK(gpu.completedValue() >= frame0Value);
        CHECK(gpu.completedValue() < gpu.signaledValue()); // frame 1 may still run
        endFrame(device);
    }

    TEST_CASE("no wait happens when the GPU has kept up") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        for (int frame = 0; frame < 4; ++frame) {
            emptyFrame(device);
        }
        CHECK(gpu.waitCount() == 0);
    }

    TEST_CASE("beginFrame while a frame is open and endFrame without one are refused") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const ith::test::AssertCapture capture;
        const Expected<void, Error> noFrame = device.endFrame();
        REQUIRE_FALSE(noFrame.hasValue());
        CHECK(noFrame.error().code == ErrorCode::InvalidUsage);
        beginFrame(device);
        const Expected<std::optional<Frame>, Error> second = device.beginFrame();
        REQUIRE_FALSE(second.hasValue());
        CHECK(second.error().code == ErrorCode::InvalidUsage);
        endFrame(device);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 2);
#endif
    }

    TEST_CASE("waitIdle waits for everything submitted") {
        NullGpu gpu;
        gpu.setAutoComplete(false);
        Device device = makeDevice(gpu);
        emptyFrame(device);
        emptyFrame(device);
        REQUIRE(device.waitIdle().hasValue());
        CHECK(gpu.completedValue() == gpu.signaledValue());
    }
}

TEST_SUITE("rhi.deferred_destruction") {
    TEST_CASE("an object is released only after the GPU finished the work submitted before its destruction") {
        NullGpu gpu;
        gpu.setAutoComplete(false);
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);
        CHECK(gpu.liveObjectCount() == 1);

        beginFrame(device);
        CommandList& list = device.openCommandList("Uses the buffer");
        list.barrier(BufferBarrier{.buffer = buffer});
        REQUIRE(device.submit(list).hasValue());
        endFrame(device);
        device.destroy(buffer); // the GPU has finished nothing yet
        CHECK(device.pendingDestructionCount() == 1);

        emptyFrame(device); // frame 1 doesn't wait, so nothing is released
        CHECK(gpu.liveObjectCount() == 1);

        beginFrame(device); // frame 2 waits for frame 0, which covers the submission
        CHECK(gpu.liveObjectCount() == 0);
        CHECK(device.pendingDestructionCount() == 0);
        endFrame(device);
    }

    TEST_CASE("an object destroyed while a command list is open waits for that list's submission") {
        NullGpu gpu;
        gpu.setAutoComplete(false);
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);

        beginFrame(device);
        CommandList& earlier = device.openCommandList("Earlier");
        REQUIRE(device.submit(earlier).hasValue());
        gpu.complete(gpu.signaledValue()); // the GPU has finished everything so far

        CommandList& open = device.openCommandList("Open while destroying");
        device.destroy(buffer);
        REQUIRE(device.submit(open).hasValue());
        const std::uint64_t submission = gpu.signaledValue();
        endFrame(device);

        // Everything before the destruction has completed, but not the list that was open then.
        emptyFrame(device);
        CHECK(gpu.completedValue() < submission);
        CHECK(gpu.liveObjectCount() == 1);

        gpu.complete(submission);
        emptyFrame(device);
        CHECK(gpu.liveObjectCount() == 0);
    }

    TEST_CASE("a discarded list releases its hold on destroyed objects") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);
        beginFrame(device);
        CommandList& list = device.openCommandList("Discarded");
        device.destroy(buffer);
        device.discard(list);
        endFrame(device);
        REQUIRE(device.waitIdle().hasValue());
        CHECK(gpu.liveObjectCount() == 0);
    }

    TEST_CASE("every object type goes through deferred destruction") {
        NullGpu gpu;
        gpu.setAutoComplete(false);
        Device device = makeDevice(gpu);
        const BufferHandle buffer = makeBuffer(device);
        const TextureHandle texture = ith::rhi::test::makeTexture(device);
        const Expected<SamplerHandle, Error> sampler = device.createSampler({.debugName = "Sampler"});
        REQUIRE(sampler.hasValue());
        const PipelineHandle pipeline = ith::rhi::test::makeComputePipeline(device);
        emptyFrame(device);
        device.destroy(buffer);
        device.destroy(texture);
        device.destroy(*sampler);
        device.destroy(pipeline);
        CHECK(device.pendingDestructionCount() == 4);
        CHECK(gpu.liveObjectCount() == 4);
        REQUIRE(device.waitIdle().hasValue());
        CHECK(device.pendingDestructionCount() == 0);
        CHECK(gpu.liveObjectCount() == 0);
    }
}

TEST_SUITE("rhi.swapchain") {
    TEST_CASE("a frame with a swapchain draws into its images in turn and presents") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 640, .height = 360, .debugName = "Main window"});
        REQUIRE(swapchain.hasValue());
        CHECK(device.isValid(*swapchain));

        std::set<std::uint64_t> images;
        for (int frameIndex = 0; frameIndex < 6; ++frameIndex) {
            const Frame frame = beginFrame(device, *swapchain);
            CHECK(device.isValid(frame.backbuffer));
            CHECK(frame.width == 640);
            CHECK(frame.height == 360);
            CHECK(frame.backbufferFormat == Format::B8G8R8A8Srgb);
            images.insert(frame.backbuffer.index());
            endFrame(device);
        }
        CHECK(images.size() == 3);
        CHECK(gpu.presentCount() == 6);
    }

    TEST_CASE("a zero size draws nothing, and a new size recreates the images under new handles") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 640, .height = 360, .debugName = "Main window"});
        REQUIRE(swapchain.hasValue());
        const TextureHandle before = beginFrame(device, *swapchain).backbuffer;
        endFrame(device);

        device.resizeSwapchain(*swapchain, 0, 0); // minimized
        const Expected<std::optional<Frame>, Error> minimized = device.beginFrame(*swapchain);
        REQUIRE(minimized.hasValue());
        CHECK_FALSE(minimized->has_value());

        device.resizeSwapchain(*swapchain, 800, 600);
        const Frame frame = beginFrame(device, *swapchain);
        CHECK(frame.width == 800);
        CHECK(frame.height == 600);
        CHECK_FALSE(device.isValid(before));
        CHECK(device.isValid(frame.backbuffer));
        CHECK(gpu.swapchainBuildCount() == 2);
        endFrame(device);
    }

    TEST_CASE("a failed recreation stales the old images, and the next frame tries again") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());
        const TextureHandle before = beginFrame(device, *swapchain).backbuffer;
        endFrame(device);

        device.resizeSwapchain(*swapchain, 128, 128);
        gpu.failNextResize();
        const Expected<std::optional<Frame>, Error> failed = device.beginFrame(*swapchain);
        REQUIRE_FALSE(failed.hasValue());
        CHECK(failed.error().code == ErrorCode::OutOfMemory);
        CHECK_FALSE(device.isLost());
        CHECK_FALSE(device.isValid(before));

        const Frame frame = beginFrame(device, *swapchain);
        CHECK(frame.width == 128);
        CHECK(device.isValid(frame.backbuffer));
        endFrame(device);
    }

    TEST_CASE("an out-of-date swapchain is recreated inside beginFrame") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());

        gpu.outdateNextAcquire();
        const Frame first = beginFrame(device, *swapchain);
        CHECK(gpu.swapchainBuildCount() == 2);
        gpu.outdateNextPresent();
        endFrame(device);

        const Frame second = beginFrame(device, *swapchain);
        CHECK(gpu.swapchainBuildCount() == 3);
        CHECK_FALSE(device.isValid(first.backbuffer));
        CHECK(device.isValid(second.backbuffer));
        endFrame(device);
    }

    TEST_CASE("destroying a swapchain makes it and its images stale at once") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());
        const TextureHandle image = beginFrame(device, *swapchain).backbuffer;
        endFrame(device);
        CHECK(gpu.liveObjectCount() == 1);

        device.destroy(*swapchain);
        CHECK_FALSE(device.isValid(*swapchain));
        CHECK_FALSE(device.isValid(image));
        CHECK(gpu.liveObjectCount() == 0);
    }

    TEST_CASE("swapchain images can't be destroyed on their own") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());
        const TextureHandle image = beginFrame(device, *swapchain).backbuffer;
        const ith::test::AssertCapture capture;
        device.destroy(image);
        CHECK(device.isValid(image));
        device.destroy(*swapchain); // the swapchain of the open frame
        CHECK(device.isValid(*swapchain));
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 2);
#endif
        endFrame(device);
    }

    TEST_CASE("a full texture table refuses a swapchain") {
        NullGpu gpu;
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.limits.textures = kMaxSwapchainImages - 1;
        Device device = makeDevice(gpu, capabilities);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE_FALSE(swapchain.hasValue());
        CHECK(swapchain.error().code == ErrorCode::OutOfHandles);
        // Nothing was kept: every texture slot is still free.
        for (std::uint32_t texture = 0; texture < capabilities.limits.textures; ++texture) {
            ith::rhi::test::makeTexture(device);
        }
    }
}

TEST_SUITE("rhi.device_lost") {
    TEST_CASE("a lost device reports DeviceLost from submit and from every later call") {
        NullGpu gpu;
        {
            Device device = makeDevice(gpu);
            makeBuffer(device);
            beginFrame(device);
            CommandList& list = device.openCommandList("Hangs");
            gpu.loseDevice("TDR");
            const Expected<void, Error> submitted = device.submit(list);
            REQUIRE_FALSE(submitted.hasValue());
            CHECK(submitted.error().code == ErrorCode::DeviceLost);
            CHECK(submitted.error().message.find("TDR") != std::string::npos);
            CHECK(device.isLost());

            const Expected<void, Error> ended = device.endFrame();
            REQUIRE_FALSE(ended.hasValue());
            CHECK(ended.error().code == ErrorCode::DeviceLost);

            const Expected<std::optional<Frame>, Error> next = device.beginFrame();
            REQUIRE_FALSE(next.hasValue());
            CHECK(next.error().code == ErrorCode::DeviceLost);

            const Expected<BufferHandle, Error> buffer =
                device.createBuffer({.size = 4, .usage = BufferUsage::ShaderRead, .debugName = "After"});
            REQUIRE_FALSE(buffer.hasValue());
            CHECK(buffer.error().code == ErrorCode::DeviceLost);

            const Expected<void, Error> idle = device.waitIdle();
            REQUIRE_FALSE(idle.hasValue());
            CHECK(idle.error().code == ErrorCode::DeviceLost);
        }
        CHECK(gpu.liveObjectCount() == 0); // a lost device still releases everything
    }

    TEST_CASE("device loss during the frame wait is reported by beginFrame") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        emptyFrame(device);
        gpu.loseDevice("page fault");
        const Expected<std::optional<Frame>, Error> frame = device.beginFrame();
        REQUIRE_FALSE(frame.hasValue());
        CHECK(frame.error().code == ErrorCode::DeviceLost);
        CHECK(frame.error().toString().find("page fault") != std::string::npos);
    }

    TEST_CASE("device loss at present closes the frame and is reported by endFrame") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const Expected<SwapchainHandle, Error> swapchain =
            device.createSwapchain({.width = 64, .height = 64, .debugName = "Window"});
        REQUIRE(swapchain.hasValue());
        beginFrame(device, *swapchain);
        gpu.loseDevice("removed");
        const Expected<void, Error> ended = device.endFrame();
        REQUIRE_FALSE(ended.hasValue());
        CHECK(ended.error().code == ErrorCode::DeviceLost);
        CHECK(device.isLost());
        device.destroy(*swapchain); // destroying works on a lost device
        CHECK_FALSE(device.isValid(*swapchain));
    }
}
