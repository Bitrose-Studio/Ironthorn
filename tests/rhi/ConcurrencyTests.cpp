// The RHI's thread-safe calls racing the render thread. These tests mean most under
// ThreadSanitizer (the linux-tsan preset), which reports any unsynchronized access they reach.

#include "ith/foundation/Assert.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/NullDevice.h"

#include "RhiTestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

using namespace ith::rhi;
using ith::Expected;
using ith::rhi::test::beginFrame;
using ith::rhi::test::endFrame;
using ith::rhi::test::makeBuffer;
using ith::rhi::test::makeComputePipeline;
using ith::rhi::test::makeDevice;
using ith::rhi::test::makeTexture;

namespace {

constexpr int kFrames = 200;

std::atomic<int> g_asserts{0};

ith::AssertAction countAssert(const ith::AssertInfo& /*info*/) {
    g_asserts.fetch_add(1, std::memory_order_relaxed);
    return ith::AssertAction::Continue;
}

// Counts failed asserts from any thread and continues, unlike ith::test::AssertCapture, which is
// for one thread.
class CountAsserts {
public:
    CountAsserts() : m_previous(ith::setAssertHandler(&countAssert)) { g_asserts.store(0); }

    ~CountAsserts() { ith::setAssertHandler(m_previous); }

    CountAsserts(const CountAsserts&) = delete;
    CountAsserts& operator=(const CountAsserts&) = delete;
    CountAsserts(CountAsserts&&) = delete;
    CountAsserts& operator=(CountAsserts&&) = delete;

private:
    ith::AssertHandler m_previous;
};

// Runs `frames` empty frames on the calling (render) thread.
void runFrames(Device& device, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        beginFrame(device);
        endFrame(device);
    }
}

} // namespace

TEST_SUITE("rhi.concurrency") {
    TEST_CASE("workers create and destroy resources while the render thread runs frames") {
        NullGpu gpu;
        // Room for every object the workers create, even if no frame released any of them: the
        // test is about races, not capacity.
        Capabilities capabilities = ith::rhi::test::testCapabilities();
        capabilities.limits.buffers = 1024;
        capabilities.limits.textures = 1024;
        capabilities.limits.resourceDescriptors = 2048;
        Device device = makeDevice(gpu, capabilities);
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 3; ++worker) {
            workers.emplace_back([&device, &failed] {
                for (int round = 0; round < 100; ++round) {
                    const Expected<BufferHandle, Error> buffer = device.createBuffer(
                        {.size = 64, .usage = BufferUsage::ShaderRead, .debugName = "Streamed buffer"});
                    const Expected<TextureHandle, Error> texture = device.createTexture({.width = 8,
                                                                                         .height = 8,
                                                                                         .format = Format::R8Unorm,
                                                                                         .usage = TextureUsage::Sampled,
                                                                                         .debugName = "Streamed"});
                    if (!buffer || !texture || device.shaderReadIndex(*buffer) == kInvalidDescriptorIndex ||
                        device.sampledIndex(*texture) == kInvalidDescriptorIndex) {
                        failed.store(true);
                    }
                    if (buffer) {
                        device.destroy(*buffer);
                    }
                    if (texture) {
                        device.destroy(*texture);
                    }
                }
            });
        }
        runFrames(device, kFrames);
        for (std::thread& worker : workers) {
            worker.join();
        }
        CHECK_FALSE(failed.load());
        REQUIRE(device.waitIdle().hasValue());
        CHECK(gpu.liveObjectCount() == 0);
    }

    TEST_CASE("workers query live handles while the render thread runs frames and releases others") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const BufferHandle upload = makeBuffer(device, BufferUsage::CopySrc, MemoryType::Upload, 256);
        const BufferHandle storage = makeBuffer(device, BufferUsage::ShaderRead | BufferUsage::ShaderWrite);
        const TextureHandle texture = makeTexture(device, TextureUsage::Sampled | TextureUsage::Storage);
        const std::uint32_t readIndex = device.shaderReadIndex(storage);
        std::atomic<bool> stop{false};
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 3; ++worker) {
            workers.emplace_back([&] {
                while (!stop.load()) {
                    const std::span<std::byte> bytes = device.mappedMemory(upload);
                    if (bytes.size() != 256 || !device.isValid(texture) ||
                        device.shaderReadIndex(storage) != readIndex ||
                        device.storageIndex(texture) == kInvalidDescriptorIndex) {
                        failed.store(true);
                    }
                }
            });
        }
        // Other objects come and go on the render thread meanwhile.
        for (int frame = 0; frame < kFrames; ++frame) {
            beginFrame(device);
            device.destroy(makeBuffer(device));
            endFrame(device);
        }
        stop.store(true);
        for (std::thread& worker : workers) {
            worker.join();
        }
        CHECK_FALSE(failed.load());
    }

    TEST_CASE("command lists opened and discarded across frame boundaries keep the counts right") {
        NullGpu gpu;
        Device device = makeDevice(gpu);
        const PipelineHandle compute = makeComputePipeline(device);
        const CountAsserts asserts; // lists opened outside a frame, or closed by endFrame, assert
        std::atomic<bool> stop{false};
        std::thread worker([&] {
            while (!stop.load()) {
                CommandList& list = device.openCommandList("Racing the frame");
                list.setPipeline(compute);
                list.dispatch(1);
                device.discard(list);
            }
        });
        runFrames(device, kFrames);
        stop.store(true);
        worker.join();
        // Every list was closed by its discard or by endFrame(), so nothing is counted as open and
        // a final frame ends cleanly.
        beginFrame(device);
        const int before = g_asserts.load();
        endFrame(device);
        CHECK(g_asserts.load() == before);
        REQUIRE(device.waitIdle().hasValue());
    }
}
