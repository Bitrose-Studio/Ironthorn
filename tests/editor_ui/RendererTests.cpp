// The editor UI's renderer backend on the RHI's null device (ADR-0029 rules 9, 10 and 18): ImGui's
// texture requests (create, partial update, destroy), the upload buffer growing, scissors clamped
// to the target, and the commands the two render graph passes record.

#include "ith/editor_ui/EditorUi.h"

#include "AssertCapture.h"
#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <thread>

using namespace ith;
using namespace ith::test::ui;

namespace {

void drawOnlyImage(UiTextureId texture) {
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({64.0f, 64.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ImageBorderSize, 0.0f);
    ImGui::Begin("User texture", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Image(static_cast<ImTextureID>(texture), {64.0f, 64.0f});
    ImGui::End();
    ImGui::PopStyleVar(3);
}

} // namespace

TEST_SUITE("editor_ui.renderer") {
    TEST_CASE("the first frame creates and fills the font atlas, and quiet frames upload nothing") {
        UiFixture fixture;
        const std::uint32_t objectsBefore = fixture.gpu.liveObjectCount();
        fixture.frame([] { ImGui::TextUnformatted("Hello"); });
        const UiRenderStats first = fixture.ui->renderStats();
        CHECK(first.textures == 1);
        CHECK(first.textureUploads >= 1);
        CHECK(first.draws >= 1);
        CHECK(first.vertices > 0);
        CHECK(first.indices > 0);
        // The atlas texture and this frame's upload buffer.
        CHECK(fixture.gpu.liveObjectCount() == objectsBefore + 2);

        // The same text again needs no new glyphs.
        for (int i = 0; i < 3; ++i) {
            fixture.frame([] { ImGui::TextUnformatted("Hello"); });
        }
        CHECK(fixture.ui->renderStats().textureUploads == 0);
        CHECK(fixture.ui->renderStats().textures == 1);
    }

    TEST_CASE("new glyphs update only rectangles of the atlas") {
        UiFixture fixture;
        fixture.frame([] { ImGui::TextUnformatted("a"); });
        fixture.frame([] { ImGui::TextUnformatted("a"); });
        REQUIRE(fixture.ui->renderStats().textureUploads == 0);
        // Glyphs nobody drew yet, at a size nobody used yet: rasterized on demand.
        fixture.frame([] {
            ImGui::PushFont(nullptr, 31.0f);
            ImGui::TextUnformatted("XYZ \xC3\xA9\xC5\x91");
            ImGui::PopFont();
        });
        const UiRenderStats stats = fixture.ui->renderStats();
        CHECK(stats.textureUploads >= 1);
        CHECK(stats.textures == 1);
    }

    TEST_CASE("an atlas that grows gets a new texture, and the old one is destroyed once unused") {
        UiFixture fixture;
        std::uint32_t mostTextures = 0;
        std::uint32_t uploads = 0;
        // Many sizes of many glyphs overflow the first atlas texture.
        for (int size = 12; size < 160; size += 4) {
            fixture.frame([size] {
                ImGui::PushFont(nullptr, static_cast<float>(size));
                ImGui::TextUnformatted("ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");
                ImGui::PopFont();
            });
            mostTextures = std::max(mostTextures, fixture.ui->renderStats().textures);
            uploads += fixture.ui->renderStats().textureUploads;
        }
        CHECK(uploads > 2);
        for (int i = 0; i < 8; ++i) {
            fixture.frame([] { ImGui::TextUnformatted("small"); });
        }
        const UiRenderStats stats = fixture.ui->renderStats();
        CHECK(stats.textures >= 1);
        CHECK(stats.textures <= mostTextures);
        // Every texture ImGui still has is one the renderer holds.
        std::uint32_t live = 0;
        for (const ImTextureData* data : ImGui::GetPlatformIO().Textures) {
            if (data->Status != ImTextureStatus_Destroyed) {
                ++live;
                CHECK(data->TexID != ImTextureID_Invalid);
            }
        }
        CHECK(live == stats.textures);
    }

    TEST_CASE("the upload buffer grows when a frame needs more, and quiet frames keep it") {
        UiFixture fixture;
        fixture.frame();
        const std::uint64_t initial = fixture.ui->renderStats().uploadBufferSize;
        CHECK(initial > 0);
        const auto lotsOfText = [] {
            ImGui::SetNextWindowPos({0.0f, 0.0f});
            ImGui::SetNextWindowSize({static_cast<float>(kWidth), static_cast<float>(kHeight)});
            ImGui::Begin("Lots of text");
            for (int line = 0; line < 400; ++line) {
                ImGui::Text("Line %d: the quick brown fox jumps over the lazy dog, again and again and again", line);
            }
            ImGui::End();
        };
        // The window's first frame only measures it; the next ones draw every line.
        fixture.frame(lotsOfText);
        fixture.frame(lotsOfText);
        fixture.frame(lotsOfText);
        const UiRenderStats grown = fixture.ui->renderStats();
        CHECK(grown.uploadBufferSize > initial);
        CHECK(grown.uploadBufferSize >= std::uint64_t{grown.vertices} * 20 + std::uint64_t{grown.indices} * 2);
    }

    TEST_CASE("scissors are clamped to the target, also for windows partly outside it") {
        UiFixture fixture;
        const auto offscreen = [] {
            ImGui::SetNextWindowPos({-60.0f, -40.0f});
            ImGui::SetNextWindowSize({300.0f, 200.0f});
            ImGui::Begin("Off the top left");
            ImGui::TextUnformatted("partly outside");
            ImGui::End();
            ImGui::SetNextWindowPos({static_cast<float>(kWidth) - 100.0f, static_cast<float>(kHeight) - 50.0f});
            ImGui::SetNextWindowSize({300.0f, 200.0f});
            ImGui::Begin("Off the bottom right");
            ImGui::TextUnformatted("partly outside");
            ImGui::End();
        };
        fixture.frame(offscreen);
        fixture.frame(offscreen);
        const UiRenderStats stats = fixture.ui->renderStats();
        REQUIRE(stats.draws > 0);
        CHECK(stats.scissorBounds.x >= 0);
        CHECK(stats.scissorBounds.y >= 0);
        CHECK(stats.scissorBounds.x + static_cast<std::int64_t>(stats.scissorBounds.width) <= kWidth);
        CHECK(stats.scissorBounds.y + static_cast<std::int64_t>(stats.scissorBounds.height) <= kHeight);
        // Both windows reach the target's edges.
        CHECK(stats.scissorBounds.x == 0);
        CHECK(stats.scissorBounds.x + static_cast<std::int64_t>(stats.scissorBounds.width) == kWidth);
    }

    TEST_CASE("the passes record a pipeline, an index buffer and a draw with scissor and constants per command") {
        UiFixture fixture;
        fixture.frame([] { ImGui::TextUnformatted("Hello"); });
        fixture.frame([] { ImGui::TextUnformatted("Hello"); });
        const std::uint64_t before = fixture.gpu.commandCount();
        fixture.frame([] { ImGui::TextUnformatted("Hello"); });
        const UiRenderStats stats = fixture.ui->renderStats();
        REQUIRE(stats.draws > 0);
        REQUIRE(stats.textureUploads == 0);
        // Per draw: scissor, constants, draw. Once: pipeline and index buffer. The graph's own
        // commands (rendering, barriers) come on top.
        CHECK(fixture.gpu.commandCount() - before >= 2 + std::uint64_t{stats.draws} * 3);
    }

    TEST_CASE("a user texture draws through one import shared by the scene and UI passes") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::RenderTarget,
                                    .debugName = "Viewport colour"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        const UiTextureId user = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(user != 0);

        fixture.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}, .deltaSeconds = 1.0f / 60.0f});
        drawOnlyImage(user);
        fixture.ui->endFrame();
        Expected<std::optional<rhi::Frame>, rhi::Error> begun = fixture.device->beginFrame();
        REQUIRE(begun.hasValue());
        REQUIRE(begun->has_value());
        fixture.graph.reset();
        const rg::Texture target = fixture.graph.importTexture(
            {.texture = fixture.target, .desc = fixture.targetDesc, .state = &fixture.targetState, .writable = true});
        const rg::Texture imported = fixture.ui->importUserTexture(fixture.graph, user);
        REQUIRE_FALSE(imported.isNull());
        CHECK(fixture.ui->importUserTexture(fixture.graph, user) == imported);
        bool sceneRecorded = false;
        fixture.graph.addPass(
            "Viewport scene", rg::PassType::Raster,
            [&](rg::PassBuilder& pass) { pass.renderTarget(imported, rhi::LoadOp::Clear); },
            [&](rg::PassContext& context) {
                sceneRecorded = true;
                CHECK(context.texture(imported) == *handle);
            });
        fixture.ui->addRenderPasses(fixture.graph, target, (*begun)->slot, rhi::LoadOp::Clear);
        fixture.graph.compile();
        const std::string dump = fixture.graph.dump();
        CHECK(dump.find("use texture 'Viewport colour': RenderTarget") != std::string::npos);
        CHECK(dump.find("use texture 'Viewport colour': Sampled") != std::string::npos);
        CHECK(dump.find("barrier texture 'Viewport colour': RenderTarget -> ShaderRead; (ColorOutput, "
                        "RenderTargetWrite) -> (PixelShader, ShaderRead)") != std::string::npos);
        Expected<void, rhi::Error> executed = fixture.graph.execute(*fixture.pool);
        REQUIRE_MESSAGE(executed.hasValue(), (executed ? std::string() : executed.error().toString()));
        REQUIRE(fixture.device->endFrame().hasValue());
        CHECK(sceneRecorded);
        CHECK(fixture.ui->renderStats().draws == 1);
        CHECK(fixture.ui->renderStats().textures == 1);

        fixture.graph.reset();
        fixture.ui->unregisterUserTexture(user);
        fixture.device->destroy(*handle);
    }

    TEST_CASE("registering after passes are built preserves the atlas's graph state") {
        UiFixture fixture;
        fixture.frame([] { ImGui::TextUnformatted("Atlas"); });
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "Late user texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());

        fixture.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}, .deltaSeconds = 1.0f / 60.0f});
        ImGui::TextUnformatted("Atlas");
        fixture.ui->endFrame();
        Expected<std::optional<rhi::Frame>, rhi::Error> begun = fixture.device->beginFrame();
        REQUIRE(begun.hasValue());
        REQUIRE(begun->has_value());
        fixture.graph.reset();
        const rg::Texture target = fixture.graph.importTexture(
            {.texture = fixture.target, .desc = fixture.targetDesc, .state = &fixture.targetState, .writable = true});
        fixture.ui->addRenderPasses(fixture.graph, target, (*begun)->slot, rhi::LoadOp::Clear);
        const UiTextureId user = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(user != 0);
        fixture.graph.compile();
        Expected<void, rhi::Error> executed = fixture.graph.execute(*fixture.pool);
        REQUIRE_MESSAGE(executed.hasValue(), (executed ? std::string() : executed.error().toString()));
        REQUIRE(fixture.device->endFrame().hasValue());

        // A new glyph writes the atlas again. Its previous ShaderRead state must have survived the
        // registration's table growth; a stale state pointer would show up here and under sanitizers.
        fixture.frame([] { ImGui::TextUnformatted("Atlas \xC3\xA9"); });
        const std::string dump = fixture.graph.dump();
        CHECK(dump.find("barrier texture 'Editor UI texture': ShaderRead -> CopyDst; (PixelShader, ShaderRead) -> "
                        "(Copy, CopyWrite)") != std::string::npos);
        fixture.graph.reset();
        fixture.ui->unregisterUserTexture(user);
        fixture.device->destroy(*handle);
    }

    TEST_CASE("an abandoned user-texture import does not leak into the next frame") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "Abandoned import"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        const UiTextureId user = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(user != 0);

        fixture.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}, .deltaSeconds = 1.0f / 60.0f});
        drawOnlyImage(user);
        fixture.ui->endFrame();
        fixture.graph.reset();
        const rg::Texture abandoned = fixture.ui->importUserTexture(fixture.graph, user);
        REQUIRE_FALSE(abandoned.isNull());
        fixture.graph.reset();
        // No addRenderPasses() this frame. The next ordinary frame must start with fresh handles.
        fixture.frame([] { ImGui::TextUnformatted("Next frame"); });
        CHECK(fixture.ui->renderStats().draws > 0);
        CHECK(fixture.graph.dump().find("Abandoned import") == std::string::npos);

        fixture.graph.reset();
        fixture.ui->unregisterUserTexture(user);
        fixture.device->destroy(*handle);
    }

    TEST_CASE("a registered texture that is neither drawn nor explicitly imported stays out of the graph") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "Unused user texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        const UiTextureId user = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(user != 0);
        fixture.frame([] { ImGui::TextUnformatted("Only the atlas"); });
        CHECK(fixture.ui->renderStats().draws > 0);
        CHECK(fixture.graph.dump().find("Unused user texture") == std::string::npos);

        fixture.graph.reset();
        fixture.ui->unregisterUserTexture(user);
        fixture.device->destroy(*handle);
    }

    TEST_CASE("unregistered, stale and destroyed user textures skip draws, and the atlas still works") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "User texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        const UiTextureId first = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(first != 0);
        fixture.frame([first] { drawOnlyImage(first); });
        CHECK(fixture.ui->renderStats().draws == 1);
        CHECK(fixture.ui->renderStats().textures == 1);

        fixture.ui->unregisterUserTexture(first);
        fixture.frame([first] { drawOnlyImage(first); });
        CHECK(fixture.ui->renderStats().draws == 0);

        const UiTextureId replacement = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(replacement != 0);
        CHECK(replacement != first);
        fixture.frame([first] { drawOnlyImage(first); });
        CHECK(fixture.ui->renderStats().draws == 0);
        fixture.frame([replacement] { drawOnlyImage(replacement); });
        CHECK(fixture.ui->renderStats().draws == 1);
        fixture.ui->unregisterUserTexture(replacement);

        const UiTextureId destroyed = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(destroyed != 0);
        // Defensive path: destroying first is tolerated, the draw is skipped, and unregister is safe.
        fixture.device->destroy(*handle);
        fixture.frame([destroyed] { drawOnlyImage(destroyed); });
        CHECK(fixture.ui->renderStats().draws == 0);
        fixture.ui->unregisterUserTexture(destroyed);

        fixture.frame([] { ImGui::TextUnformatted("The font atlas still works"); });
        CHECK(fixture.ui->renderStats().draws > 0);
        CHECK(fixture.ui->renderStats().textures == 1);
    }

    TEST_CASE("unregistering and registering after the passes are built leaves the pending graph intact") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "Original user texture"};
        rhi::TextureDesc replacementDesc = desc;
        replacementDesc.debugName = "Replacement user texture";
        Expected<rhi::TextureHandle, rhi::Error> original = fixture.device->createTexture(desc);
        Expected<rhi::TextureHandle, rhi::Error> replacementHandle = fixture.device->createTexture(replacementDesc);
        REQUIRE(original.hasValue());
        REQUIRE(replacementHandle.hasValue());
        const UiTextureId user = fixture.ui->registerUserTexture(*original, desc);
        REQUIRE(user != 0);

        fixture.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}, .deltaSeconds = 1.0f / 60.0f});
        drawOnlyImage(user);
        fixture.ui->endFrame();
        Expected<std::optional<rhi::Frame>, rhi::Error> begun = fixture.device->beginFrame();
        REQUIRE(begun.hasValue());
        REQUIRE(begun->has_value());
        fixture.graph.reset();
        const rg::Texture target = fixture.graph.importTexture(
            {.texture = fixture.target, .desc = fixture.targetDesc, .state = &fixture.targetState, .writable = true});
        fixture.ui->addRenderPasses(fixture.graph, target, (*begun)->slot, rhi::LoadOp::Clear);

        const ith::test::AssertCapture capture;
        // Between building and executing the graph: the freed slot must not be handed out again.
        fixture.ui->unregisterUserTexture(user);
        const UiTextureId replacement = fixture.ui->registerUserTexture(*replacementHandle, replacementDesc);
        REQUIRE(replacement != 0);
        CHECK((replacement & 0xFFFF'FFFFu) != (user & 0xFFFF'FFFFu));

        fixture.graph.compile();
        const std::string dump = fixture.graph.dump();
        CHECK(dump.find("use texture 'Original user texture': Sampled") != std::string::npos);
        CHECK(dump.find("Replacement user texture") == std::string::npos);
        Expected<void, rhi::Error> executed = fixture.graph.execute(*fixture.pool);
        REQUIRE_MESSAGE(executed.hasValue(), (executed ? std::string() : executed.error().toString()));
        REQUIRE(fixture.device->endFrame().hasValue());
        CHECK(fixture.ui->renderStats().draws == 1);
        CHECK(capture.count() == 0);

        // The next frame draws the replacement from its own slot.
        fixture.frame([replacement] { drawOnlyImage(replacement); });
        CHECK(fixture.ui->renderStats().draws == 1);
        CHECK(fixture.graph.dump().find("use texture 'Replacement user texture': Sampled") != std::string::npos);
        CHECK(capture.count() == 0);

        fixture.graph.reset();
        fixture.ui->unregisterUserTexture(replacement);
        fixture.device->destroy(*original);
        fixture.device->destroy(*replacementHandle);
    }

    TEST_CASE("a user texture's initial state is the before state of its first barrier") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::RenderTarget,
                                    .debugName = "Prefilled user texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        // As a graph that rendered into it would have written it back.
        const rg::ImportedState rendered{.layout = rhi::TextureLayout::RenderTarget,
                                         .stages = rhi::PipelineStage::ColorOutput,
                                         .access = rhi::Access::RenderTargetWrite};
        const UiTextureId user = fixture.ui->registerUserTexture(*handle, desc, rendered);
        REQUIRE(user != 0);

        fixture.frame([user] { drawOnlyImage(user); });
        CHECK(fixture.ui->renderStats().draws == 1);
        CHECK(fixture.graph.dump().find("barrier texture 'Prefilled user texture': RenderTarget -> ShaderRead; "
                                        "(ColorOutput, RenderTargetWrite) -> (PixelShader, ShaderRead)") !=
              std::string::npos);

        fixture.graph.reset();
        fixture.ui->unregisterUserTexture(user);
        fixture.device->destroy(*handle);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("passes for another graph than the user-texture imports change nothing") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "Mismatched user texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        const UiTextureId user = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(user != 0);

        // The first frame: the font atlas still has to be created and uploaded.
        fixture.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}, .deltaSeconds = 1.0f / 60.0f});
        ImGui::TextUnformatted("Atlas");
        fixture.ui->endFrame();
        fixture.graph.reset();
        REQUIRE_FALSE(fixture.ui->importUserTexture(fixture.graph, user).isNull());
        // A reset gives the graph a new serial, so the target belongs to another graph.
        fixture.graph.reset();
        const rg::Texture target = fixture.graph.importTexture(
            {.texture = fixture.target, .desc = fixture.targetDesc, .state = &fixture.targetState, .writable = true});
        {
            const ith::test::AssertCapture capture;
            fixture.ui->addRenderPasses(fixture.graph, target, 0, rhi::LoadOp::Clear);
            CHECK(capture.count() == 1);
        }
        fixture.graph.compile();
        CHECK(fixture.graph.dump().find("EditorUi") == std::string::npos);
        CHECK(fixture.ui->renderStats().textureUploads == 0);
        fixture.graph.reset();

        // Nothing was marked uploaded: the next ordinary frame still creates and fills the atlas.
        fixture.frame([] { ImGui::TextUnformatted("Atlas"); });
        CHECK(fixture.ui->renderStats().textureUploads >= 1);
        CHECK(fixture.ui->renderStats().textures == 1);
        CHECK(fixture.ui->renderStats().draws > 0);

        fixture.graph.reset();
        fixture.ui->unregisterUserTexture(user);
        fixture.device->destroy(*handle);
    }

    TEST_CASE("user-texture calls from another thread than the creating one assert") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "Threaded user texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        const UiTextureId user = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(user != 0);
        {
            const ith::test::AssertCapture capture;
            std::thread other([&fixture, user] { fixture.ui->unregisterUserTexture(user); });
            other.join();
            CHECK(capture.count() == 1);
        }
        fixture.device->destroy(*handle);
    }

    TEST_CASE("registration changes during graph imports and invalid unregistrations assert") {
        UiFixture fixture;
        fixture.frame([] { ImGui::TextUnformatted("Atlas"); });
        const ImTextureID atlas = ImGui::GetIO().Fonts->TexRef.GetTexID();
        REQUIRE(atlas != ImTextureID_Invalid);
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "User texture"};
        Expected<rhi::TextureHandle, rhi::Error> firstHandle = fixture.device->createTexture(desc);
        Expected<rhi::TextureHandle, rhi::Error> secondHandle = fixture.device->createTexture(desc);
        REQUIRE(firstHandle.hasValue());
        REQUIRE(secondHandle.hasValue());
        const UiTextureId first = fixture.ui->registerUserTexture(*firstHandle, desc);
        REQUIRE(first != 0);

        fixture.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}, .deltaSeconds = 1.0f / 60.0f});
        drawOnlyImage(first);
        fixture.ui->endFrame();
        {
            const ith::test::AssertCapture capture;
            CHECK(fixture.ui->registerUserTexture(*secondHandle, desc) == 0);
            CHECK(capture.count() == 1);
            fixture.ui->unregisterUserTexture(first);
            CHECK(capture.count() == 2);
        }

        Expected<std::optional<rhi::Frame>, rhi::Error> begun = fixture.device->beginFrame();
        REQUIRE(begun.hasValue());
        REQUIRE(begun->has_value());
        fixture.graph.reset();
        const rg::Texture target = fixture.graph.importTexture(
            {.texture = fixture.target, .desc = fixture.targetDesc, .state = &fixture.targetState, .writable = true});
        fixture.ui->addRenderPasses(fixture.graph, target, (*begun)->slot, rhi::LoadOp::Clear);
        fixture.graph.compile();
        Expected<void, rhi::Error> executed = fixture.graph.execute(*fixture.pool);
        REQUIRE_MESSAGE(executed.hasValue(), (executed ? std::string() : executed.error().toString()));
        REQUIRE(fixture.device->endFrame().hasValue());
        CHECK(fixture.ui->renderStats().draws == 1);
        fixture.graph.reset();
        {
            const ith::test::AssertCapture capture;
            fixture.ui->unregisterUserTexture(0);
            CHECK(capture.count() == 1);
            fixture.ui->unregisterUserTexture(atlas);
            CHECK(capture.count() == 2);
            fixture.ui->unregisterUserTexture(first);
            CHECK(capture.count() == 2);
            fixture.ui->unregisterUserTexture(first);
            CHECK(capture.count() == 3);
        }
        fixture.device->destroy(*firstHandle);
        fixture.device->destroy(*secondHandle);
    }

    TEST_CASE("user texture registration rejects integer and 32-bit float formats and missing debug names") {
        UiFixture fixture;
        rhi::TextureDesc desc{.width = 64,
                              .height = 64,
                              .format = rhi::Format::R32Uint,
                              .usage = rhi::TextureUsage::Sampled,
                              .debugName = "Integer user texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        int expected = 0;
        {
            const ith::test::AssertCapture capture;
            CHECK(fixture.ui->registerUserTexture(*handle, desc) == 0);
            ++expected;
            CHECK(capture.count() == expected);
            desc.format = rhi::Format::R32G32Uint; // the viewport's id target's format
            CHECK(fixture.ui->registerUserTexture(*handle, desc) == 0);
            ++expected;
            CHECK(capture.count() == expected);
            // Not guaranteed to filter linearly on Vulkan, and the UI's sampler is linear.
            for (const rhi::Format format :
                 {rhi::Format::R32Float, rhi::Format::R32G32Float, rhi::Format::R32G32B32A32Float}) {
                desc.format = format;
                CHECK(fixture.ui->registerUserTexture(*handle, desc) == 0);
                ++expected;
                CHECK(capture.count() == expected);
            }
            desc.format = rhi::Format::R8G8B8A8Unorm;
            desc.debugName = {};
            CHECK(fixture.ui->registerUserTexture(*handle, desc) == 0);
            ++expected;
            CHECK(capture.count() == expected);
        }
        fixture.device->destroy(*handle);
    }
#endif

    TEST_CASE("a stale user id cannot draw after ImGui takes its table slot") {
        UiFixture fixture;
        const rhi::TextureDesc desc{.width = 64,
                                    .height = 64,
                                    .format = rhi::Format::R8G8B8A8Unorm,
                                    .usage = rhi::TextureUsage::Sampled,
                                    .debugName = "Stale user texture"};
        Expected<rhi::TextureHandle, rhi::Error> handle = fixture.device->createTexture(desc);
        REQUIRE(handle.hasValue());
        const UiTextureId stale = fixture.ui->registerUserTexture(*handle, desc);
        REQUIRE(stale != 0);
        fixture.frame([stale] { drawOnlyImage(stale); });
        fixture.ui->unregisterUserTexture(stale);

        const ImTextureID originalAtlas = ImGui::GetIO().Fonts->TexRef.GetTexID();
        ImTextureID atlas = originalAtlas;
        for (int size = 12; size < 160 && atlas == originalAtlas; size += 4) {
            fixture.frame([size] {
                ImGui::PushFont(nullptr, static_cast<float>(size));
                ImGui::TextUnformatted("ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");
                ImGui::PopFont();
            });
            atlas = ImGui::GetIO().Fonts->TexRef.GetTexID();
        }
        REQUIRE(atlas != originalAtlas);
        bool slotTakenByImGui = false;
        for (const ImTextureData* data : ImGui::GetPlatformIO().Textures) {
            slotTakenByImGui =
                slotTakenByImGui || (data->TexID == ImTextureID{1} && data->Status != ImTextureStatus_Destroyed);
        }
        CHECK(slotTakenByImGui);
        for (int i = 0; i < 4; ++i) {
            fixture.frame([] { ImGui::TextUnformatted("atlas"); });
        }
        REQUIRE(ImGui::GetIO().Fonts->TexRef.GetTexID() == atlas);
        REQUIRE(fixture.ui->renderStats().textures == 1);

        fixture.frame([stale] { drawOnlyImage(stale); });
        CHECK(fixture.ui->renderStats().draws == 0);
        CHECK(fixture.ui->renderStats().textureUploads == 0);
        CHECK(fixture.ui->renderStats().textures == 1);
        CHECK(ImGui::GetIO().Fonts->TexRef.GetTexID() == atlas);
        CHECK(fixture.graph.dump().find("texture 'Editor UI texture'") == std::string::npos);
        fixture.frame([] { ImGui::TextUnformatted("atlas okay"); });
        CHECK(fixture.ui->renderStats().draws > 0);
        CHECK(ImGui::GetIO().Fonts->TexRef.GetTexID() == atlas);
        fixture.device->destroy(*handle);
    }

    TEST_CASE("the UI's GPU objects are released with it") {
        std::uint32_t baseline = 0;
        {
            UiFixture fixture;
            baseline = fixture.gpu.liveObjectCount();
            fixture.frame([] { ImGui::TextUnformatted("Hello"); });
            CHECK(fixture.gpu.liveObjectCount() > baseline);
            fixture.ui.reset();
            // Deferred destruction completes over the frames in flight.
            for (std::uint32_t i = 0; i <= rhi::kFramesInFlight; ++i) {
                REQUIRE(fixture.device->beginFrame().hasValue());
                REQUIRE(fixture.device->endFrame().hasValue());
            }
            // The pipeline and the sampler went too.
            CHECK(fixture.gpu.liveObjectCount() == baseline - 2);
        }
    }
}
