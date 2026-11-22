// The editor UI's renderer backend on the RHI's null device (ADR-0029 rules 9, 10 and 18): ImGui's
// texture requests (create, partial update, destroy), the upload buffer growing, scissors clamped
// to the target, and the commands the two render graph passes record.

#include "ith/editor_ui/EditorUi.h"

#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <cstdint>
#include <string>

using namespace ith;
using namespace ith::test::ui;

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
