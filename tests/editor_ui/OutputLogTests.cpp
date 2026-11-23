// The Output Log panel: records taken from the ring buffer once each, the level filter, the search,
// Clear, records the buffer overwrote counted as missed, and the panel drawn docked on the null
// device.

#include "ith/editor_ui/OutputLog.h"

#include "ith/editor_core/PanelRegistry.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"

#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <string>
#include <string_view>
#include <vector>

using namespace ith;
using namespace ith::test::ui;

namespace {

constinit LogCategory g_render{"Render"};
constinit LogCategory g_assets{"Assets"};

void write(LogRingBuffer& buffer, LogLevel level, const LogCategory& category, std::string_view message) {
    buffer.write({.level = level, .category = &category, .message = message, .file = __FILE__, .line = __LINE__});
}

std::vector<std::string> messages(const OutputLog& log) {
    std::vector<std::string> result;
    for (const LogEntry* entry : log.visible()) {
        result.push_back(entry->message);
    }
    return result;
}

} // namespace

TEST_SUITE("editor_ui.output_log") {
    TEST_CASE("records are taken once, filtered by level and searched without case") {
        LogRingBuffer buffer(8);
        OutputLog log(buffer, defaultDarkTheme());
        write(buffer, LogLevel::Info, g_render, "Swapchain resized");
        write(buffer, LogLevel::Warning, g_assets, "Texture too large");
        log.update();
        write(buffer, LogLevel::Error, g_render, "Device lost");
        log.update();
        log.update();
        CHECK(messages(log) == std::vector<std::string>{"Swapchain resized", "Texture too large", "Device lost"});

        log.setFilter(OutputLogFilter::WarningsAndErrors);
        CHECK(messages(log) == std::vector<std::string>{"Texture too large", "Device lost"});
        log.setFilter(OutputLogFilter::Errors);
        CHECK(messages(log) == std::vector<std::string>{"Device lost"});
        log.setFilter(OutputLogFilter::All);
        log.setSearch("RENDER");
        CHECK(messages(log) == std::vector<std::string>{"Swapchain resized", "Device lost"});
        log.setSearch("too");
        CHECK(messages(log) == std::vector<std::string>{"Texture too large"});
        log.setSearch("");
        log.clear();
        CHECK(log.visible().empty());
        write(buffer, LogLevel::Info, g_render, "After clear");
        log.update();
        CHECK(messages(log) == std::vector<std::string>{"After clear"});
    }

    TEST_CASE("records overwritten before they were taken count as missed, and the log keeps the buffer's size") {
        LogRingBuffer buffer(4);
        OutputLog log(buffer, defaultDarkTheme());
        for (int i = 0; i < 10; ++i) {
            write(buffer, LogLevel::Info, g_render, std::to_string(i));
        }
        log.update();
        CHECK(log.missed() == 6);
        CHECK(messages(log) == std::vector<std::string>{"6", "7", "8", "9"});
        for (int i = 10; i < 13; ++i) {
            write(buffer, LogLevel::Info, g_render, std::to_string(i));
            log.update();
        }
        CHECK(messages(log) == std::vector<std::string>{"9", "10", "11", "12"});
    }

    TEST_CASE("the panel draws its records in a window") {
        UiFixture fixture;
        LogRingBuffer buffer(64);
        OutputLog log(buffer, fixture.ui->theme());
        PanelRegistry panels;
        registerOutputLog(panels, log);
        const PanelDesc* panel = panels.find("ith.output_log");
        REQUIRE(panel != nullptr);
        CHECK(panel->defaultDock == DockPlace::Bottom);
        for (int i = 0; i < 100; ++i) {
            write(buffer, i % 3 == 0 ? LogLevel::Warning : LogLevel::Info, g_render, "line " + std::to_string(i));
        }
        for (int frame = 0; frame < 3; ++frame) {
            fixture.frame([&] {
                ImGui::SetNextWindowSize({600.0f, 300.0f});
                ImGui::Begin("Output Log###ith.output_log");
                panel->draw("ith.output_log");
                ImGui::End();
            });
        }
        CHECK(log.visible().size() == 64);
        CHECK(fixture.ui->renderStats().draws > 0);
    }
}
