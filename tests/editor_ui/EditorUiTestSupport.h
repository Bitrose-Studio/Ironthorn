#pragma once

// Helpers of the editor_ui tests: an EditorUi on the RHI's null device, with an Input, a UiHost
// that records what the UI asks of the platform, and frames rendered through a render graph into
// an offscreen target. Headless: no window, no GPU (ADR-0029 rule 18).

#include "ith/editor_ui/EditorUi.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/input/Input.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/NullDevice.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <utility>
#include <vector>

namespace ith::test::ui {

// The window id the tests' events carry.
inline constexpr WindowId kWindow = 1;
inline constexpr std::int32_t kWidth = 1280;
inline constexpr std::int32_t kHeight = 720;

// Bytes that stand in for the compiled shaders; the null device takes any.
inline constexpr std::array<std::byte, 4> kShaderCode{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

// The fonts folder the build fills next to the test executable.
inline std::string fontDirectory() {
    const Expected<std::string> directory = fs::executableDirectory();
    REQUIRE(directory.hasValue());
    return fs::joinPath(*directory, "fonts");
}

// Records the platform calls of the UI, and serves a clipboard.
class RecordingHost final : public UiHost {
public:
    [[nodiscard]] std::string clipboardText() override {
        ++clipboardReads;
        return clipboard;
    }

    void setClipboardText(std::string_view text) override { clipboard = std::string(text); }

    void setTextInputArea(const TextInputArea& area) override { textInputArea = area; }

    void setCursor(std::optional<CursorShape> shape) override {
        cursor = shape;
        ++cursorChanges;
    }

    void setRelativeMouseMode(bool enabled) override {
        relativeMouseMode = enabled;
        ++relativeMouseModeChanges;
        mouseCalls.push_back(enabled ? "relative on" : "relative off");
    }

    void warpMouse(float x, float y) override {
        warpedTo = {x, y};
        mouseCalls.push_back("warp");
    }

    std::string clipboard;
    int clipboardReads = 0;
    std::optional<TextInputArea> textInputArea;
    std::optional<CursorShape> cursor;
    int cursorChanges = 0;
    bool relativeMouseMode = false;
    int relativeMouseModeChanges = 0;
    // Where the last warp put the cursor, in window pixels.
    std::optional<std::pair<float, float>> warpedTo;
    // The relative-mode switches and warps, in order.
    std::vector<std::string> mouseCalls;
};

// A consumer below the UI that records what reaches it.
class RecordingConsumer final : public InputConsumer {
public:
    InputReply onInputEvent(const Event& event) override {
        events.push_back(event.type);
        return InputReply::NotConsumed;
    }

    [[nodiscard]] bool saw(EventType type) const {
        for (const EventType seen : events) {
            if (seen == type) {
                return true;
            }
        }
        return false;
    }

    std::vector<EventType> events;
};

// An EditorUi on a null device, rendering into an offscreen sRGB target of kWidth x kHeight.
// `belowUi`, when given, is added to the input stack under the UI's consumer (the keymap consumer).
struct UiFixture {
    explicit UiFixture(EditorUiDesc overrides = {}, InputConsumer* belowUi = nullptr) {
        Expected<rhi::Device, rhi::Error> created = rhi::createNullDevice({.gpu = &gpu});
        REQUIRE(created.hasValue());
        device.emplace(std::move(*created));
        pool.emplace(*device);
        Expected<rhi::TextureHandle, rhi::Error> texture = device->createTexture(targetDesc);
        REQUIRE(texture.hasValue());
        target = *texture;
        below = input.addConsumer({.window = kWindow, .layer = ConsumerLayer::Editor, .consumer = &lower});
        if (belowUi != nullptr) {
            input.addConsumer({.window = kWindow, .layer = ConsumerLayer::Editor, .consumer = belowUi});
        }
        overrides.device = &*device;
        overrides.targetFormat = targetDesc.format;
        overrides.vertexShader = kShaderCode;
        overrides.pixelShader = kShaderCode;
        fonts = fontDirectory();
        overrides.fontDirectory = fonts;
        overrides.input = &input;
        overrides.window = kWindow;
        if (overrides.host == nullptr) {
            overrides.host = &host;
        }
        Expected<std::unique_ptr<EditorUi>> made = EditorUi::create(overrides);
        REQUIRE_MESSAGE(made.hasValue(), (made ? std::string() : made.error().message));
        ui = std::move(*made);
        // The window has focus, as after start-up.
        events({Event{.type = EventType::WindowFocusGained, .window = kWindow}});
    }

    UiFixture(const UiFixture&) = delete;
    UiFixture& operator=(const UiFixture&) = delete;
    UiFixture(UiFixture&&) = delete;
    UiFixture& operator=(UiFixture&&) = delete;

    ~UiFixture() {
        ui.reset();
        graph.reset();
        pool.reset();
        device->destroy(target);
    }

    // Routes one frame's events through Input.
    void events(std::vector<Event> list) {
        timeNs += 16'000'000;
        for (Event& event : list) {
            event.timestampNs = timeNs;
            if (event.window == kNoWindow) {
                event.window = kWindow;
            }
        }
        input.processEvents(list, timeNs);
    }

    // One UI frame: `draw` runs between newFrame and endFrame.
    template <typename Draw>
    void frame(Draw&& draw, bool focused = true) {
        ui->newFrame(
            {.size = {kWidth, kHeight}, .pixelSize = pixelSize, .deltaSeconds = 1.0f / 60.0f, .focused = focused});
        draw();
        ui->endFrame();
        render();
    }

    void frame() {
        frame([] {});
    }

    // Renders the last UI frame into the target through the render graph.
    void render() {
        Expected<std::optional<rhi::Frame>, rhi::Error> begun = device->beginFrame();
        REQUIRE(begun.hasValue());
        REQUIRE(begun->has_value());
        graph.reset();
        const rg::Texture texture =
            graph.importTexture({.texture = target, .desc = targetDesc, .state = &targetState, .writable = true});
        if (beforeUi) {
            beforeUi(graph);
        }
        ui->addRenderPasses(graph, texture, (*begun)->slot, rhi::LoadOp::Clear);
        graph.compile();
        lastGraph = graph.dump();
        Expected<void, rhi::Error> executed = graph.execute(*pool);
        REQUIRE_MESSAGE(executed.hasValue(), (executed ? std::string() : executed.error().toString()));
        REQUIRE(device->endFrame().hasValue());
    }

    rhi::NullGpu gpu;
    std::optional<rhi::Device> device;
    std::optional<rg::ResourcePool> pool;
    rg::RenderGraph graph{{.name = "Editor UI test"}};
    rhi::TextureDesc targetDesc{.width = kWidth,
                                .height = kHeight,
                                .format = rhi::Format::R8G8B8A8Srgb,
                                .usage = rhi::TextureUsage::RenderTarget,
                                .debugName = "Editor UI target"};
    rhi::TextureHandle target;
    rg::ImportedState targetState;
    Input input;
    RecordingConsumer lower;
    ConsumerId below = kNoConsumer;
    RecordingHost host;
    std::string fonts;
    std::unique_ptr<EditorUi> ui;
    // The window's size in pixels that frame() hands the UI; twice the window size is a display scale
    // of 2.
    WindowSize pixelSize{kWidth, kHeight};
    // Adds passes to the graph after the UI frame and before the UI's passes (viewport scene passes).
    std::function<void(rg::RenderGraph&)> beforeUi;
    // The dump of the last rendered graph, compiled.
    std::string lastGraph;
    std::uint64_t timeNs = 1'000'000'000;
};

} // namespace ith::test::ui
