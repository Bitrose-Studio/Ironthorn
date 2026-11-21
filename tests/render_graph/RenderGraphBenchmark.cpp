// The render graph benchmark (ADR-0028 rule 24): declaring, compiling and executing a synthetic
// graph of 100 passes and 200 resources, on the null backend. It only prints numbers; the initial
// target is a compile under 0.1 ms on the Low desktop tier's CPU, reported, not enforced.

#include "ith/foundation/Stopwatch.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/NullDevice.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace ith;

constexpr int kPasses = 100;
constexpr int kIterations = 2000;

// 100 compute passes; pass i writes transients 2i and 2i + 1 and reads two of the earlier ones,
// and the last pass also writes an imported buffer, so every pass reaches the output.
void declare(rg::RenderGraph& graph, rg::Buffer output, std::vector<rg::Texture>& textures) {
    textures.clear();
    for (int i = 0; i < kPasses; ++i) {
        const rg::Texture a =
            graph.createTexture({.width = 256,
                                 .height = 256,
                                 .format = i % 2 == 0 ? rhi::Format::R16G16B16A16Float : rhi::Format::R8G8B8A8Unorm,
                                 .debugName = "Transient"});
        const rg::Texture b = graph.createTexture(
            {.width = 128, .height = 128, .format = rhi::Format::R32Float, .debugName = "Transient"});
        graph.addPass(
            "Pass", rg::PassType::Compute,
            [&](rg::PassBuilder& pass) {
                if (i > 0) {
                    pass.read(textures[textures.size() - 1], rg::TextureUse::Sampled);
                }
                if (i > 1) {
                    pass.read(textures[textures.size() - 4], rg::TextureUse::Sampled);
                }
                pass.write(a, rg::TextureUse::Storage);
                pass.write(b, rg::TextureUse::Storage);
                if (i + 1 == kPasses) {
                    pass.write(output, rg::BufferUse::Storage);
                }
            },
            [](rg::PassContext& /*context*/) {});
        textures.push_back(a);
        textures.push_back(b);
    }
}

double median(std::vector<double>& values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

int main() {
    rhi::Capabilities capabilities;
    capabilities.limits.textures = 1024;
    capabilities.limits.buffers = 64;
    Expected<rhi::Device, rhi::Error> created = rhi::createNullDevice({.capabilities = capabilities});
    if (!created) {
        std::fprintf(stderr, "render_graph_benchmark: %s\n", created.error().toString().c_str());
        return 1;
    }
    rhi::Device device = std::move(created).value();
    Expected<rhi::BufferHandle, rhi::Error> outputBuffer =
        device.createBuffer({.size = 256, .usage = rhi::BufferUsage::ShaderWrite, .debugName = "Output"});
    if (!outputBuffer) {
        return 1;
    }
    rg::ResourcePool pool(device);
    rg::RenderGraph graph({.name = "Benchmark"});
    std::vector<rg::Texture> textures;
    textures.reserve(2 * kPasses);
    std::vector<double> declareTimes;
    std::vector<double> compileTimes;
    std::vector<double> executeTimes;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        Stopwatch watch;
        graph.reset();
        const rg::Buffer output =
            graph.importBuffer({.buffer = *outputBuffer,
                                .desc = {.size = 256, .usage = rhi::BufferUsage::ShaderWrite, .debugName = "Output"},
                                .writable = true});
        declare(graph, output, textures);
        declareTimes.push_back(watch.elapsedMilliseconds());

        watch.restart();
        graph.compile();
        compileTimes.push_back(watch.elapsedMilliseconds());

        Expected<std::optional<rhi::Frame>, rhi::Error> frame = device.beginFrame();
        if (!frame || !frame->has_value()) {
            return 1;
        }
        watch.restart();
        const Expected<void, rhi::Error> executed = graph.execute(pool);
        executeTimes.push_back(watch.elapsedMilliseconds());
        const Expected<void, rhi::Error> ended = device.endFrame();
        if (!executed || !ended) {
            std::fprintf(stderr, "render_graph_benchmark: execute failed\n");
            return 1;
        }
    }
    graph.reset();
    std::printf("render_graph_benchmark: %d passes, %d resources, %d iterations, %zu pooled textures\n", kPasses,
                2 * kPasses + 1, kIterations, pool.textureCount());
    std::printf("  declare  median %.4f ms\n", median(declareTimes));
    std::printf("  compile  median %.4f ms (target < 0.1 ms on the Low desktop tier's CPU)\n", median(compileTimes));
    std::printf("  execute  median %.4f ms (null backend: pool, barriers, recording calls, submit)\n",
                median(executeTimes));
    pool.clear();
    device.destroy(*outputBuffer);
    static_cast<void>(device.waitIdle());
    return 0;
}
