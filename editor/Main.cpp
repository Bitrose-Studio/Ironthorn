// ironthorn_editor: the Ironthorn editor (ADR-0008, ADR-0029, ADR-0040, ADR-0049). It opens a window
// with the editor shell - the main menu with the File, Edit and Window commands and their shortcuts,
// the dock space with the panels (Viewport, Outliner, Details, Content Browser, Output Log) in the
// last session's layout, the content drawer on Ctrl + Space, and the status bar - over one editor
// session of a project, whose Content folder is scanned when it opens.
//
//   ironthorn_editor --project <dir>            edits the project in <dir>
//   ironthorn_editor ... --user-dir <dir>       the user configuration layer (default: the user's
//                                               configuration folder, Ironthorn/Editor)
//   ironthorn_editor ... --automation <script>  runs an automation script (ADR-0037 rule 19) after
//                                               the first frames, then exits: 0 when it passed, 1
//                                               when it failed
//   ironthorn_editor ... --frames N             exits after N frames
//   ironthorn_editor ... --backend B            renders with backend B: d3d12, vulkan or null
//                                               (default: the platform's)
//   ironthorn_editor ... --adapter A            hardware (the default) or software (WARP, lavapipe)
//
// The loop follows ADR-0035 rule 22: events (waiting while idle, ADR-0029 rule 12), main-thread
// jobs, input routing, the UI frame, rendering. Where the window has no native handle (SDL's
// headless "offscreen" driver, which CTest uses) the UI renders into an offscreen texture; where no
// GPU device can be created it renders on the null device, so automation always runs.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Importer.h"
#include "ith/asset_importers/GltfImporter.h"
#include "ith/config/Config.h"
#include "ith/editor_core/Automation.h"
#include "ith/editor_core/EditorPreferences.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/ViewportInput.h"
#include "ith/editor_ui/ContentBrowserPanel.h"
#include "ith/editor_ui/DetailsPanel.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/EditorShell.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/editor_ui/EditorUiShader.h"
#include "ith/editor_ui/IdlePolicy.h"
#include "ith/editor_ui/OutlinerPanel.h"
#include "ith/editor_ui/OutputLog.h"
#include "ith/editor_ui/Theme.h"
#include "ith/editor_ui/ViewportPanel.h"
#include "ith/editor_ui/ViewportShaders.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/foundation/Profile.h"
#include "ith/foundation/Version.h"
#include "ith/input/Input.h"
#include "ith/jobs/JobSystem.h"
#include "ith/platform/Platform.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/NullDevice.h"
#include "ith/rhi_select/CreateDevice.h"
#include "ith/shader_compiler/ShaderCompiler.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)
ITH_DECLARE_REFLECTION_MODULE(shapes)

namespace {

using Clock = std::chrono::steady_clock;

constexpr ith::rhi::Format kTargetFormat = ith::rhi::Format::B8G8R8A8Srgb;
// The log records the Output Log keeps.
constexpr std::size_t kLogHistory = 4096;

#ifdef _WIN32
constexpr std::string_view kPlatformName = "Windows";
#else
constexpr std::string_view kPlatformName = "Linux";
#endif

int fail(const ith::Error& error) {
    std::fprintf(stderr, "ironthorn_editor: %s (%s)\n", error.message.c_str(), error.code.c_str());
    return 1;
}

void warn(const ith::Error& error) {
    std::fprintf(stderr, "ironthorn_editor: warning: %s (%s)\n", error.message.c_str(), error.code.c_str());
}

// The command line.
struct Options {
    std::string project;
    std::string userDirectory;
    std::string automation;
    // 0 runs until the window is closed.
    std::uint64_t frameLimit = 0;
    std::optional<ith::rhi::Backend> backend;
    ith::rhi::AdapterPreference adapter = ith::rhi::AdapterPreference::HighPerformance;
};

constexpr const char* kUsage = "usage: ironthorn_editor --project <dir> [--user-dir <dir>] [--automation <script>] "
                               "[--frames N] [--backend d3d12|vulkan|null] [--adapter hardware|software]\n";

std::optional<ith::rhi::Backend> parseBackend(std::string_view value) {
    if (value == "d3d12") {
        return ith::rhi::Backend::D3D12;
    }
    if (value == "vulkan") {
        return ith::rhi::Backend::Vulkan;
    }
    if (value == "null") {
        return ith::rhi::Backend::Null;
    }
    return std::nullopt;
}

// The options, or std::nullopt after printing what is wrong with them.
std::optional<Options> parseOptions(std::span<char* const> arguments) {
    Options options;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string_view argument(arguments[i]);
        const bool hasValue = i + 1 < arguments.size();
        const std::string_view value = hasValue ? std::string_view(arguments[i + 1]) : std::string_view();
        if (!hasValue || value.empty()) {
            std::fputs(kUsage, stderr);
            return std::nullopt;
        }
        ++i;
        if (argument == "--project") {
            options.project = ith::fs::normalizePath(value);
        } else if (argument == "--user-dir") {
            options.userDirectory = ith::fs::normalizePath(value);
        } else if (argument == "--automation") {
            options.automation = std::string(value);
        } else if (argument == "--frames") {
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), options.frameLimit);
            if (error != std::errc() || end != value.data() + value.size() || options.frameLimit == 0) {
                std::fprintf(stderr, "ironthorn_editor: --frames needs a positive number\n");
                return std::nullopt;
            }
        } else if (argument == "--backend") {
            options.backend = parseBackend(value);
            if (!options.backend) {
                std::fprintf(stderr, "ironthorn_editor: --backend takes d3d12, vulkan or null\n");
                return std::nullopt;
            }
        } else if (argument == "--adapter" && (value == "hardware" || value == "software")) {
            options.adapter = value == "software" ? ith::rhi::AdapterPreference::Software
                                                  : ith::rhi::AdapterPreference::HighPerformance;
        } else {
            std::fputs(kUsage, stderr);
            return std::nullopt;
        }
    }
    if (options.project.empty()) {
        std::fputs(kUsage, stderr);
        return std::nullopt;
    }
    return options;
}

// The configuration layers of the editor (ADR-0035 rule 12), lowest first: engine, engine
// platform, project, project platform, and the user's, which is the one written.
ith::Expected<std::vector<ith::ConfigLayer>> configLayers(const Options& options) {
    const ith::Expected<std::string> executable = ith::fs::executableDirectory();
    if (!executable) {
        return ith::makeUnexpected(executable.error());
    }
    std::string user = options.userDirectory;
    if (user.empty()) {
        const ith::Expected<std::string> base = ith::fs::userConfigDirectory();
        if (!base) {
            return ith::makeUnexpected(base.error());
        }
        user = ith::fs::joinPath(*base, "Ironthorn/Editor");
    }
    const std::string engine = ith::fs::joinPath(*executable, "Config");
    const std::string project = ith::fs::joinPath(options.project, "Config");
    return std::vector<ith::ConfigLayer>{
        {.name = "engine", .directory = engine},
        {.name = "engine_platform", .directory = ith::fs::joinPath(engine, kPlatformName)},
        {.name = "project", .directory = project},
        {.name = "project_platform", .directory = ith::fs::joinPath(project, kPlatformName)},
        {.name = "user", .directory = user, .writable = true},
    };
}

// The device the UI renders with: the chosen or the platform's backend, or the null device when no
// GPU device can be created, so the editor and its automation still run.
ith::rhi::Device createEditorDevice(const Options& options) {
    ith::Expected<ith::rhi::Device, ith::rhi::Error> device =
        ith::rhi::createDevice({.adapter = options.adapter, .applicationName = "ironthorn_editor"}, options.backend);
    if (device) {
        return std::move(*device);
    }
    std::fprintf(stderr, "ironthorn_editor: %s; rendering on the null device\n", device.error().toString().c_str());
    ith::Expected<ith::rhi::Device, ith::rhi::Error> null = ith::rhi::createNullDevice({});
    ITH_ASSERT_MSG(null.hasValue(), "the null device can always be created");
    return std::move(*null);
}

// The editor's shader stages for the device (ADR-0029 rule 9, ADR-0042 rule 17): the UI's and the
// viewport's shapes and grid passes, in SPIR-V or DXIL, and SPIR-V for the null device, which takes
// any bytes, so its runs check the shaders too.
struct EditorShaders {
    std::vector<std::byte> uiVertex;
    std::vector<std::byte> uiPixel;
    std::vector<std::byte> shapesVertex;
    std::vector<std::byte> shapesPixel;
    std::vector<std::byte> gridVertex;
    std::vector<std::byte> gridPixel;
};

ith::Expected<EditorShaders> compileEditorShaders(ith::rhi::ShaderFormat format) {
    ith::Expected<ith::ShaderCompiler> compiler = ith::ShaderCompiler::create();
    if (!compiler) {
        return ith::makeUnexpected(compiler.error());
    }
    const ith::ShaderTarget target =
        format == ith::rhi::ShaderFormat::Dxil ? ith::ShaderTarget::Dxil : ith::ShaderTarget::Spirv;

    struct Stage {
        std::string_view name;
        std::string_view source;
        std::string_view entry;
        ith::ShaderStage stage;
        std::vector<std::byte>* output;
    };

    EditorShaders shaders;
    const std::array stages{
        Stage{ith::kEditorUiShaderName, ith::editorUiShaderSource(), ith::kEditorUiVertexEntry,
              ith::ShaderStage::Vertex, &shaders.uiVertex},
        Stage{ith::kEditorUiShaderName, ith::editorUiShaderSource(), ith::kEditorUiPixelEntry, ith::ShaderStage::Pixel,
              &shaders.uiPixel},
        Stage{ith::kViewportShapesShaderName, ith::viewportShapesShaderSource(), ith::kViewportShapesVertexEntry,
              ith::ShaderStage::Vertex, &shaders.shapesVertex},
        Stage{ith::kViewportShapesShaderName, ith::viewportShapesShaderSource(), ith::kViewportShapesPixelEntry,
              ith::ShaderStage::Pixel, &shaders.shapesPixel},
        Stage{ith::kViewportGridShaderName, ith::viewportGridShaderSource(), ith::kViewportGridVertexEntry,
              ith::ShaderStage::Vertex, &shaders.gridVertex},
        Stage{ith::kViewportGridShaderName, ith::viewportGridShaderSource(), ith::kViewportGridPixelEntry,
              ith::ShaderStage::Pixel, &shaders.gridPixel},
    };
    for (const Stage& stage : stages) {
        ith::Expected<std::vector<std::byte>> code = compiler->compile({.name = stage.name,
                                                                        .source = stage.source,
                                                                        .entryPoint = stage.entry,
                                                                        .stage = stage.stage,
                                                                        .target = target});
        if (!code) {
            return ith::makeUnexpected(code.error());
        }
        *stage.output = std::move(*code);
    }
    return shaders;
}

// Where the UI draws: the window's swapchain, or an offscreen texture of the window's size when the
// window has no native handle the device can present to.
class RenderTarget {
public:
    RenderTarget(ith::rhi::Device& device, const ith::Window& window) : m_device(&device) {
        const ith::WindowSize pixels = window.pixelSize();
        ith::Expected<ith::rhi::SwapchainHandle, ith::rhi::Error> swapchain =
            device.createSwapchain({.window = window.nativeHandle(),
                                    .width = static_cast<std::uint32_t>(pixels.width),
                                    .height = static_cast<std::uint32_t>(pixels.height),
                                    .format = kTargetFormat,
                                    .debugName = "Editor window"});
        if (swapchain) {
            m_swapchain = *swapchain;
        } else {
            std::printf("ironthorn_editor: %s; drawing offscreen\n", swapchain.error().toString().c_str());
            resize(pixels);
        }
    }

    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    RenderTarget(RenderTarget&&) = delete;
    RenderTarget& operator=(RenderTarget&&) = delete;

    // The swapchain goes before the window; the device waits for the GPU first.
    ~RenderTarget() {
        static_cast<void>(m_device->waitIdle());
        m_device->destroy(m_swapchain);
        m_device->destroy(m_texture);
    }

    [[nodiscard]] ith::rhi::SwapchainHandle swapchain() const { return m_swapchain; }

    // Follows the window's size in pixels; zero draws nothing.
    void resize(ith::WindowSize pixels) {
        if (m_swapchain != ith::rhi::SwapchainHandle{}) {
            m_device->resizeSwapchain(m_swapchain, static_cast<std::uint32_t>(pixels.width),
                                      static_cast<std::uint32_t>(pixels.height));
            return;
        }
        m_device->destroy(m_texture);
        m_texture = {};
        m_state = {};
        if (pixels.width <= 0 || pixels.height <= 0) {
            return;
        }
        m_desc = {.width = static_cast<std::uint32_t>(pixels.width),
                  .height = static_cast<std::uint32_t>(pixels.height),
                  .format = kTargetFormat,
                  .usage = ith::rhi::TextureUsage::RenderTarget,
                  .debugName = "Editor offscreen target"};
        if (ith::Expected<ith::rhi::TextureHandle, ith::rhi::Error> texture = m_device->createTexture(m_desc)) {
            m_texture = *texture;
        }
    }

    // The frame's target in `graph`, or a null handle when nothing can be drawn.
    ith::rg::Texture import(ith::rg::RenderGraph& graph, const ith::rhi::Frame& frame) {
        if (m_swapchain != ith::rhi::SwapchainHandle{}) {
            return graph.importBackbuffer(frame);
        }
        if (m_texture == ith::rhi::TextureHandle{}) {
            return {};
        }
        return graph.importTexture({.texture = m_texture, .desc = m_desc, .state = &m_state, .writable = true});
    }

private:
    ith::rhi::Device* m_device;
    ith::rhi::SwapchainHandle m_swapchain;
    ith::rhi::TextureHandle m_texture;
    ith::rhi::TextureDesc m_desc;
    ith::rg::ImportedState m_state;
};

// Renders the UI's last frame, after the viewports' scene passes in the same graph. Returns false
// when the device is lost or the frame failed.
bool render(ith::rhi::Device& device, RenderTarget& target, ith::rg::RenderGraph& graph, ith::rg::ResourcePool& pool,
            ith::EditorUi& ui, ith::ViewportPanel& viewports) {
    ith::Expected<std::optional<ith::rhi::Frame>, ith::rhi::Error> frame = device.beginFrame(target.swapchain());
    if (!frame) {
        std::fprintf(stderr, "ironthorn_editor: %s\n", frame.error().toString().c_str());
        return false;
    }
    if (!frame->has_value()) {
        // Minimized: the UI frame is not drawn. ImGui keeps its texture requests for the next one.
        return true;
    }
    graph.reset();
    const ith::rg::Texture texture = target.import(graph, **frame);
    ith::Expected<void, ith::rhi::Error> executed;
    if (!texture.isNull()) {
        viewports.addRenderPasses(graph, **frame);
        ui.addRenderPasses(graph, texture, (*frame)->slot, ith::rhi::LoadOp::Clear,
                           ith::linearColor(ui.theme().background));
        graph.compile();
        executed = graph.execute(pool);
    }
    const ith::Expected<void, ith::rhi::Error> ended = device.endFrame();
    if (!executed || !ended) {
        std::fprintf(stderr, "ironthorn_editor: %s\n",
                     (!executed ? executed.error() : ended.error()).toString().c_str());
        return false;
    }
    return true;
}

// The window's pixels per window coordinate; 1 while it has no size (minimized).
float pixelDensity(const ith::Window& window) {
    const ith::WindowSize size = window.size();
    const ith::WindowSize pixels = window.pixelSize();
    return size.width > 0 && pixels.width > 0 ? static_cast<float>(pixels.width) / static_cast<float>(size.width)
                                              : 1.0f;
}

// Seconds between two times, for the UI's time step.
float secondsBetween(Clock::time_point earlier, Clock::time_point later) {
    return std::chrono::duration<float>(later - earlier).count();
}

// Whether a close request may end the editor. Unsaved changes keep it open once (ADR-0037 rule
// 15), with the reason in the status bar; until the save dialog exists, closing again discards them.
bool mayClose(const ith::EditorSession& session, ith::EditorShellState& shell, bool& warned) {
    if (!session.document().dirty() || warned) {
        return true;
    }
    warned = true;
    shell.lastResult = ith::makeError("editor.document.unsaved_changes",
                                      "The level has unsaved changes: save it, or close again to discard them");
    shell.status = shell.lastResult.error().message;
    return false;
}

// Applies what input decided to the window: text input for a focused text field. The UI applies
// relative mode, while the mouse is captured or a viewport flies, through its host.
void applyInput(const ith::Input& input, ith::Window& window) {
    if (input.textInputWanted(window.id()) != window.textInputActive()) {
        if (window.textInputActive()) {
            window.stopTextInput();
        } else {
            static_cast<void>(window.startTextInput());
        }
    }
}

// Ends the session at exit: a save still writing finishes (ADR-0037 rule 16), what the user arranged
// since the last read-back goes into the session layout, the open level's viewport cameras are
// remembered for the next session (ADR-0042 rule 4), and the session's warnings are printed.
void finishSession(ith::EditorSession& session, ith::DockLayout& dock) {
    session.waitIdle();
    dock.readBack();
    session.layouts().saveSession();
    session.saveViewports();
    for (const ith::Error& warning : session.takeWarnings()) {
        warn(warning);
    }
}

// Scans the project's Content folder, so the Content Browser shows the disk as it is when the project
// opens (spec 3.14); a failure is a warning, and the browser stays empty until a Rescan works.
void scanContent(ith::EditorSession& session) {
    if (const ith::Expected<ith::serialization::Value> scanned = session.execute("ith.content.rescan"); !scanned) {
        warn(scanned.error());
    }
}

// Runs the automation script at `path` and waits for its file operations; false when it failed.
bool runAutomation(ith::EditorSession& session, const std::string& path) {
    ith::Automation automation(session);
    const ith::Expected<ith::ScriptReport> report = automation.runScriptFile(path);
    automation.waitIdle();
    if (!report) {
        std::fprintf(stderr, "ironthorn_editor: automation failed: %s\n", report.error().message.c_str());
        return false;
    }
    std::printf("ironthorn_editor: automation %s passed: %zu steps\n", path.c_str(), report->steps);
    return true;
}

// What the idle policy watches besides events and jobs: the document, the layout, the Content
// Browser and the log.
struct StateWatch {
    std::uint64_t document = 0;
    std::uint64_t layout = 0;
    std::uint64_t content = 0;
    std::uint64_t log = 0;

    // True when any of them changed since the last call.
    bool changed(const ith::EditorSession& session, const ith::LogRingBuffer& history) {
        std::vector<ith::LogEntry> records;
        const std::uint64_t nextLog = history.read(log, records);
        const std::uint64_t nextDocument = session.document().changes().any();
        const std::uint64_t nextLayout = session.layouts().changes();
        const std::uint64_t nextContent = session.content().generation();
        const bool any = nextDocument != document || nextLayout != layout || nextContent != content || nextLog != log;
        document = nextDocument;
        layout = nextLayout;
        content = nextContent;
        log = nextLog;
        return any;
    }
};

// Applies an event of the editor's window: a new pixel size resizes the swapchain, and a new display
// scale rescales the UI.
void applyWindowEvent(const ith::Event& event, const ith::Window& window, RenderTarget& target, ith::EditorUi& ui,
                      ith::DockLayout& dock) {
    switch (event.type) {
        case ith::EventType::WindowPixelSizeChanged:
            target.resize({.width = event.width, .height = event.height});
            break;
        case ith::EventType::WindowDisplayScaleChanged:
            ui.setDisplayScale(window.displayScale());
            dock.displayScaleChanged();
            break;
        default:
            break;
    }
}

// Readies the viewport consumer for routing the batch `events` of `window`: drags move cameras in
// window coordinates and the click slop is in logical points, the same on every display scale; and a
// focus loss in the batch cancels a held gizmo drag and the clicks before Input synthesises the
// release of held buttons, which must neither pick nor commit (ADR-0047 item 2). The batch counts as
// a whole, so a real release just before the focus loss in the same batch is cancelled too.
void prepareViewportInput(ith::ViewportConsumer& viewportInput, const ith::Window& window,
                          std::span<const ith::Event> events) {
    viewportInput.setPixelDensity(pixelDensity(window));
    viewportInput.setDisplayScale(window.displayScale());
    if (std::ranges::any_of(events, [&window](const ith::Event& event) {
            return event.type == ith::EventType::WindowFocusLost && event.window == window.id();
        })) {
        viewportInput.focusLost();
    }
}

// The editor's frame loop and its state, once the platform, the device and the session exist.
// Fails with "editor.project_not_found" unless the project directory exists.
ith::Expected<void> checkProject(const Options& options) {
    const ith::Expected<std::optional<ith::fs::FileInfo>> projectInfo = ith::fs::fileInfo(options.project);
    if (!projectInfo || !projectInfo->has_value() || (*projectInfo)->kind != ith::fs::FileKind::Directory) {
        return ith::makeError("editor.project_not_found",
                              "the project directory " + options.project + " does not exist");
    }
    return {};
}

int runEditor(const Options& options, ith::Platform& platform, ith::JobSystem& jobs, ith::Window& window) {
    if (const ith::Expected<void> project = checkProject(options); !project) {
        return fail(project.error());
    }
    ith::Expected<std::vector<ith::ConfigLayer>> layers = configLayers(options);
    if (!layers) {
        return fail(layers.error());
    }
    ith::Config config(std::move(*layers));
    const ith::EditorPreferences preferences = config.get(ith::kEditorPreferences);

    ith::TypeRegistry types;
    ith::generated::registerReflection_ecs(types);
    ith::generated::registerReflection_editor_core(types);
    ith::generated::registerReflection_shapes(types);
    // The project's assets (ADR-0015): the importers, then the database the Content Browser scans.
    ith::ImporterRegistry importers;
    importers.add(std::make_unique<ith::GltfImporter>());
    ith::AssetDatabase assets(options.project, importers);
    ith::EditorSession session(
        types, {.projectDirectory = options.project, .jobs = &jobs, .assets = &assets, .config = &config});
    std::vector<ith::Error> warnings;
    const ith::Keymap keymap = ith::Keymap::load(config, session.commands(), warnings);
    warnings.insert(warnings.end(), config.warnings().begin(), config.warnings().end());
    for (const ith::Error& warning : warnings) {
        // Commands of later editor work (the content file operations, Play In Editor) are bound before
        // they exist.
        if (warning.code == "editor.keymap.unknown_command") {
            continue;
        }
        warn(warning);
    }

    ith::rhi::Device device = createEditorDevice(options);
    const ith::rhi::Capabilities& capabilities = device.capabilities();
    ith::Expected<EditorShaders> shaders = compileEditorShaders(capabilities.shaderFormat);
    if (!shaders) {
        return fail(shaders.error());
    }
    std::printf("ironthorn_editor: rendering on '%s'\n", capabilities.adapter.name.c_str());

    // Declared in the order they must go: the UI and its passes before the pool, the pool and the
    // target before the device.
    RenderTarget target(device, window);
    ith::rg::RenderGraph graph({.name = "Editor frame"});
    ith::rg::ResourcePool pool(device);
    ith::Input input;
    ith::KeymapConsumer shortcuts(session, keymap);
    ith::ViewportConsumer viewportInput(session);
    // The keymap consumer first, then the viewports': the UI's consumer, added last, sits above both,
    // and the viewports above the keymap (ADR-0029 rule 7, ADR-0042 rule 8).
    const ith::ConsumerId shortcutConsumer =
        input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
    const ith::ConsumerId viewportConsumer =
        input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
    const ith::Expected<std::string> executable = ith::fs::executableDirectory();
    const std::string fonts = executable ? ith::fs::joinPath(*executable, "fonts") : std::string("fonts");
    ith::WindowUiHost host(platform, window);
    ith::Expected<std::unique_ptr<ith::EditorUi>> created =
        ith::EditorUi::create({.device = &device,
                               .targetFormat = kTargetFormat,
                               .vertexShader = shaders->uiVertex,
                               .pixelShader = shaders->uiPixel,
                               .fontDirectory = fonts,
                               .input = &input,
                               .window = window.id(),
                               .host = &host,
                               .viewports = &viewportInput,
                               .content = &session.content(),
                               .displayScale = window.displayScale()});
    if (!created) {
        input.removeConsumer(viewportConsumer);
        input.removeConsumer(shortcutConsumer);
        return fail(created.error());
    }
    ith::EditorUi& ui = **created;
    ith::Expected<std::unique_ptr<ith::ViewportPanel>> viewportPanel =
        ith::ViewportPanel::create(session, ui, device,
                                   {.shapesVertex = shaders->shapesVertex,
                                    .shapesPixel = shaders->shapesPixel,
                                    .gridVertex = shaders->gridVertex,
                                    .gridPixel = shaders->gridPixel});
    if (!viewportPanel) {
        created->reset();
        input.removeConsumer(viewportConsumer);
        input.removeConsumer(shortcutConsumer);
        return fail(viewportPanel.error());
    }

    // The Output Log shows the log from here on; the dock space restores the last session's layout,
    // or the default one (ADR-0040 rule 16), once the panels are registered.
    ith::LogRingBuffer logHistory(kLogHistory);
    const ith::Expected<void> sinkAdded = ith::addLogSink(logHistory.sink());
    if (!sinkAdded) {
        warn(sinkAdded.error());
    }
    // Registered in the Window menu's order (ADR-0040 rule 3); the Outliner is also the Rename prompt.
    ith::registerViewport(session.panels(), **viewportPanel);
    ith::OutlinerPanel outliner(session);
    ith::registerOutliner(session, outliner);
    ith::DetailsPanel details(session);
    ith::registerDetails(session.panels(), details);
    ith::ContentBrowserPanel contentBrowser(session);
    ith::registerContentBrowser(session, contentBrowser);
    ith::OutputLog outputLog(logHistory, ui.theme());
    ith::registerOutputLog(session.panels(), outputLog);
    ith::DockLayout dock(session);
    static_cast<void>(session.layouts().restore());
    scanContent(session);

    ith::IdlePolicy idle(
        {.focused = preferences.idleFramesPerSecond, .unfocused = preferences.idleFramesPerSecondUnfocused});
    ith::EditorShellState shell;
    StateWatch watch;
    Clock::time_point last = Clock::now();
    std::uint64_t frames = 0;
    int status = 0;
    bool automationDone = options.automation.empty();
    bool running = true;
    // A close was refused once because of unsaved changes.
    bool closeWarned = false;
    while (running) {
        // ADR-0035 rule 22: events (waiting while idle), main-thread jobs, input, update, render.
        const std::chrono::milliseconds timeout = idle.waitTimeout(input.focusedWindow() == window.id());
        const std::span<const ith::Event> events =
            timeout.count() == 0 ? platform.pumpEvents() : platform.waitEvents(timeout);
        const std::uint32_t jobsRun = jobs.runMainThreadJobs();
        prepareViewportInput(viewportInput, window, events);
        input.processEvents(events, platform.pumpTimeNs());
        for (const ith::Event& event : events) {
            if (event.type == ith::EventType::QuitRequested ||
                (event.type == ith::EventType::WindowCloseRequested && event.window == window.id())) {
                running = !mayClose(session, shell, closeWarned);
            } else if (event.window == window.id()) {
                applyWindowEvent(event, window, target, ui, dock);
            }
        }
        applyInput(input, window);
        const Clock::time_point now = Clock::now();
        const float deltaSeconds = secondsBetween(last, now);
        last = now;
        // A moving viewport camera keeps frames coming, also while no event arrives (a held W).
        const bool cameraMoved = viewportInput.update(deltaSeconds);

        const bool changed = watch.changed(session, logHistory);
        if (!events.empty() || jobsRun > 0 || changed || cameraMoved) {
            idle.requestFrame();
        }

        ui.newFrame({.size = window.size(),
                     .pixelSize = window.pixelSize(),
                     .deltaSeconds = deltaSeconds,
                     .focused = input.focusedWindow() == window.id()});
        (*viewportPanel)->newFrame();
        ith::drawEditorShell(session, keymap, shell, dock);
        contentBrowser.drawDrawer();
        ui.endFrame();
        if (!render(device, target, graph, pool, ui, **viewportPanel)) {
            status = 1;
            break;
        }
        idle.frameFinished();
        // A waiting pick needs device frames to complete, also while no event arrives; a minimized
        // window draws none, so it waits there without spinning the loop.
        if ((*viewportPanel)->picking() && window.pixelSize().width > 0) {
            idle.requestFrame();
        }
        ++frames;

        // Automation runs once the shell has drawn a few frames, then the editor exits.
        if (!automationDone && frames >= 1 + ith::IdlePolicy::kSettleFrames) {
            if (!runAutomation(session, options.automation)) {
                status = 1;
            }
            automationDone = true;
            idle.requestFrame();
        } else if (!options.automation.empty() && automationDone && idle.pendingFrames() == 0) {
            running = false; // the frames after the script have been drawn
        }
        if (options.frameLimit != 0 && frames >= options.frameLimit) {
            running = false;
        }
        ITH_PROFILE_FRAME();
    }
    finishSession(session, dock);
    // A graph whose execution failed still holds the passes; the last UI frame may not have been
    // rendered (minimized, failed), leaving the UI in its graph-import phase. Both are closed before
    // the viewport's targets, user textures of the UI, go first.
    graph.reset();
    ui.abandonFrame();
    viewportPanel->reset();
    created->reset();
    input.removeConsumer(viewportConsumer);
    input.removeConsumer(shortcutConsumer);
    std::printf("ironthorn_editor: %llu frames\n", static_cast<unsigned long long>(frames));
    return status;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(misc-const-correctness): the signature of main is fixed
    const std::optional<Options> options = parseOptions(std::span<char* const>(argv, static_cast<std::size_t>(argc)));
    if (!options) {
        return 2;
    }
    ith::Expected<ith::Platform> platform = ith::Platform::create();
    if (!platform) {
        return fail(platform.error());
    }
    // Declared after the Platform, so it goes first: no wake reaches a Platform that shuts down.
    ith::JobSystem jobs;
    jobs.setMainThreadWake([](void* /*context*/) { ith::Platform::wake(); }, nullptr);
    ith::Expected<ith::Window> window = platform->createWindow({.title = "Ironthorn Editor"});
    if (!window) {
        return fail(window.error());
    }
    std::printf("Ironthorn %.*s editor: project %s\n", static_cast<int>(ith::kEngineVersionString.size()),
                ith::kEngineVersionString.data(), options->project.c_str());
    return runEditor(*options, *platform, jobs, *window);
}
