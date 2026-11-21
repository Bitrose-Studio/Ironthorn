// Declaring and compiling a steady frame allocates nothing (ADR-0028 rule 1): names and execute
// callables go to the graph's arena, and its tables keep their capacity across resets. This file
// replaces the global allocation functions of its own test executable with counting ones, which
// count only while a test asks them to; sanitizer builds don't build it (their runtimes replace
// the same functions).

#include "ith/render_graph/RenderGraph.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void* allocate(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        std::abort(); // the engine is built without exceptions
    }
    return memory;
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    void* memory = _aligned_malloc(size == 0 ? 1 : size, align);
#else
    void* memory = std::aligned_alloc(align, ((size + align - 1) / align) * align);
#endif
    if (memory == nullptr) {
        std::abort();
    }
    return memory;
}

void freeAligned(void* memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace

// Every form of the replaceable global allocation functions ([new.delete]), so every pair matches.
void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocateAligned(size, alignment);
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    return allocateAligned(size, alignment);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept {
    freeAligned(memory);
}

using namespace ith;
using namespace ith::rg;

namespace {

// A frame of 22 passes with long names, transients, an import, reads, writes, attachments and
// buffers; the execute callables capture more than a pointer.
void declareAndCompile(RenderGraph& graph) {
    graph.reset();
    const Texture output =
        graph.importTexture({.texture = rhi::TextureHandle::fromParts(1, 1),
                             .desc = {.width = 64,
                                      .height = 64,
                                      .format = rhi::Format::R16G16B16A16Float,
                                      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
                                      .debugName = "An imported output with a long name"},
                             .writable = true,
                             .finalLayout = rhi::TextureLayout::ShaderRead});
    const std::array<float, 8> captured{};
    Texture previous = graph.createTexture({.width = 64,
                                            .height = 64,
                                            .format = rhi::Format::R16G16B16A16Float,
                                            .debugName = "The first transient, with a long name"});
    const Buffer arguments = graph.createBuffer({.size = 256, .debugName = "Indirect arguments with a long name"});
    graph.addPass(
        "The first pass, with a long name", PassType::Compute,
        [&](PassBuilder& pass) {
            pass.write(previous, TextureUse::Storage);
            pass.write(arguments, BufferUse::Storage);
        },
        [captured](PassContext& /*context*/) { static_cast<void>(captured); });
    for (int i = 0; i < 20; ++i) {
        const Texture next = graph.createTexture({.width = 64,
                                                  .height = 64,
                                                  .format = rhi::Format::R16G16B16A16Float,
                                                  .debugName = "A transient of the chain, with a long name"});
        graph.addPass(
            "A pass of the chain, with a long name", PassType::Raster,
            [&](PassBuilder& pass) {
                pass.read(previous, TextureUse::Sampled, rhi::PipelineStage::PixelShader);
                pass.read(arguments, BufferUse::Indirect);
                pass.renderTarget(next, rhi::LoadOp::DontCare);
            },
            [captured](PassContext& /*context*/) { static_cast<void>(captured); });
        previous = next;
    }
    graph.addPass(
        "The last pass, with a long name", PassType::Raster,
        [&](PassBuilder& pass) {
            pass.read(previous, TextureUse::Sampled);
            pass.renderTarget(output, rhi::LoadOp::Load);
        },
        [](PassContext& /*context*/) {});
    graph.compile();
}

} // namespace

TEST_SUITE("render_graph.allocation") {
    TEST_CASE("declaring and compiling a steady frame allocates nothing") {
        RenderGraph graph({.name = "Steady"});
        // The first frames size the arena and the tables.
        declareAndCompile(graph);
        declareAndCompile(graph);
        REQUIRE(graph.passCount() == 22);

        g_allocations.store(0);
        g_counting.store(true);
        declareAndCompile(graph);
        g_counting.store(false);
        const std::size_t allocations = g_allocations.load();
        CHECK(allocations == 0);
        CHECK(graph.groupCount() == 3);
        graph.reset();
    }
}
