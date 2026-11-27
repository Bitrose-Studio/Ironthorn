// Files dropped from other applications and the open-file dialog (ADR-0049 rules 24, 25 and 28) on
// SDL's offscreen video driver. Drops are pushed into SDL's queue the way the operating system's
// driver sends them. The operating system's dialog can't be shown headless: the tests open it with
// an invalid filter, which SDL rejects before showing anything, or stand in for showing it and for
// its callback with ith::detail::beginOpenFileDialog and ith::detail::finishOpenFileDialog. They
// rely on ith::WindowId being SDL's window id.

#include "ith/platform/Platform.h"
#include "ith/platform/PlatformState.h"

#include "AssertCapture.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <ostream> // doctest prints std::string_view operands through it
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

ith::Platform createHeadless() {
    ith::Expected<ith::Platform> platform = ith::Platform::create({.videoDriver = "offscreen"});
    REQUIRE_MESSAGE(platform.hasValue(), (platform ? "" : platform.error().message.c_str()));
    return std::move(*platform);
}

ith::Window createWindow(ith::Platform& platform, const ith::WindowDesc& desc = {}) {
    ith::Expected<ith::Window> window = platform.createWindow(desc);
    REQUIRE_MESSAGE(window.hasValue(), (window ? "" : window.error().message.c_str()));
    return std::move(*window);
}

// An event of the pump with its text copied, so it may outlive the pump.
struct Pumped {
    ith::Event event;
    std::string text;
};

std::vector<Pumped> pump(ith::Platform& platform) {
    std::vector<Pumped> result;
    for (const ith::Event& event : platform.pumpEvents()) {
        result.push_back({.event = event, .text = std::string(event.text)});
    }
    return result;
}

void pushDrop(SDL_EventType type, ith::WindowId window, float x = 0.0f, float y = 0.0f, const char* data = nullptr) {
    SDL_Event event{};
    event.drop.type = type;
    event.drop.windowID = window;
    event.drop.x = x;
    event.drop.y = y;
    event.drop.data = data;
    REQUIRE(SDL_PushEvent(&event));
}

// A filter SDL rejects before it shows a dialog: '*' is allowed only on its own.
constexpr std::array<ith::FileDialogFilter, 1> kInvalidFilters{{{.name = "Images", .patterns = "*.png"}}};

// Ends a dialog the test left open, also when a REQUIRE failed before the test ended it, so that
// "platform.dialog_busy" can't spill into later cases. Declared before the Platform, so it runs
// after the Platform is gone; it does nothing when no dialog is open.
class DialogGuard {
public:
    DialogGuard() = default;
    DialogGuard(const DialogGuard&) = delete;
    DialogGuard& operator=(const DialogGuard&) = delete;
    DialogGuard(DialogGuard&&) = delete;
    DialogGuard& operator=(DialogGuard&&) = delete;

    ~DialogGuard() {
        const std::array<const char*, 1> none{nullptr};
        ith::detail::finishOpenFileDialog(none.data());
    }
};

} // namespace

TEST_CASE("Drops translate one to one, with their window, positions in pixels and paths") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform, {.width = 400, .height = 300});
    static_cast<void>(pump(platform));
    // Window pixels per window coordinate, which the mouse positions are scaled by too.
    const float density = static_cast<float>(window.pixelSize().width) / static_cast<float>(window.size().width);

    // Long enough to make the platform's text storage grow while the frame is gathered.
    const std::string longPath = "/home/user/" + std::string(300, 'a') + ".gltf";
    pushDrop(SDL_EVENT_DROP_BEGIN, window.id());
    pushDrop(SDL_EVENT_DROP_POSITION, window.id(), 10.0f, 20.5f);
    pushDrop(SDL_EVENT_DROP_FILE, window.id(), 10.0f, 20.5f, "C:\\Art\\\xC3\xA9t\xC3\xA9.png");
    pushDrop(SDL_EVENT_DROP_FILE, window.id(), 10.0f, 20.5f, longPath.c_str());
    // Dropped text is not delivered.
    pushDrop(SDL_EVENT_DROP_TEXT, window.id(), 10.0f, 20.5f, "some text");
    // SDL sends the last position with every drop event; only DropPosition and DropFile keep it.
    pushDrop(SDL_EVENT_DROP_COMPLETE, window.id(), 10.0f, 20.5f);

    const std::vector<Pumped> events = pump(platform);
    REQUIRE(events.size() == 5);
    CHECK(events[0].event.type == ith::EventType::DropBegin);
    CHECK(events[1].event.type == ith::EventType::DropPosition);
    CHECK(events[2].event.type == ith::EventType::DropFile);
    CHECK(events[3].event.type == ith::EventType::DropFile);
    CHECK(events[4].event.type == ith::EventType::DropComplete);
    for (const Pumped& pumped : events) {
        CHECK(pumped.event.window == window.id());
        CHECK(pumped.event.timestampNs > 0);
    }
    for (const std::size_t i : {1U, 2U, 3U}) {
        CHECK(events[i].event.x == doctest::Approx(10.0f * density));
        CHECK(events[i].event.y == doctest::Approx(20.5f * density));
    }
    CHECK(events[0].event.x == 0.0f);
    CHECK(events[4].event.x == 0.0f);
    CHECK(events[4].event.y == 0.0f);
    CHECK(events[1].text.empty());
    CHECK(events[2].text == "C:\\Art\\\xC3\xA9t\xC3\xA9.png");
    CHECK(events[3].text == longPath);
}

TEST_CASE("A drop onto the application rather than a window has no window") {
    ith::Platform platform = createHeadless();
    static_cast<void>(pump(platform));
    pushDrop(SDL_EVENT_DROP_BEGIN, 0);
    pushDrop(SDL_EVENT_DROP_FILE, 0, 0.0f, 0.0f, "/tmp/a.png");
    pushDrop(SDL_EVENT_DROP_COMPLETE, 0);

    const std::vector<Pumped> events = pump(platform);
    REQUIRE(events.size() == 3);
    CHECK(events[1].event.type == ith::EventType::DropFile);
    CHECK(events[1].text == "/tmp/a.png");
    for (const Pumped& pumped : events) {
        CHECK(pumped.event.window == ith::kNoWindow);
    }
}

TEST_CASE("A drag that carries only text sends DropBegin and DropComplete and no DropFile") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));
    pushDrop(SDL_EVENT_DROP_BEGIN, window.id());
    pushDrop(SDL_EVENT_DROP_TEXT, window.id(), 1.0f, 1.0f, "hello");
    pushDrop(SDL_EVENT_DROP_COMPLETE, window.id());

    const std::vector<Pumped> events = pump(platform);
    REQUIRE(events.size() == 2);
    CHECK(events[0].event.type == ith::EventType::DropBegin);
    CHECK(events[1].event.type == ith::EventType::DropComplete);
}

TEST_CASE("The dialog's default folder always ends in a separator") {
    CHECK(ith::detail::dialogFolder("").empty());
#ifdef _WIN32
    CHECK(ith::detail::dialogFolder("C:\\Art") == "C:\\Art\\");
    CHECK(ith::detail::dialogFolder("C:\\Art\\") == "C:\\Art\\");
    CHECK(ith::detail::dialogFolder("C:\\") == "C:\\");
    CHECK(ith::detail::dialogFolder("C:/Art/Models") == "C:\\Art\\Models\\");
    CHECK(ith::detail::dialogFolder("C:/Art/") == "C:\\Art\\");
    CHECK(ith::detail::dialogFolder("/") == "\\");
#else
    CHECK(ith::detail::dialogFolder("/home/user/Art") == "/home/user/Art/");
    CHECK(ith::detail::dialogFolder("/home/user/Art/") == "/home/user/Art/");
    CHECK(ith::detail::dialogFolder("/") == "/");
    // Backslashes are file name characters on Linux.
    CHECK(ith::detail::dialogFolder("/tmp/a\\b") == "/tmp/a\\b/");
#endif
}

TEST_CASE("The file dialog's chosen files arrive in a later pump, each in its event, then FileDialogClosed") {
    const DialogGuard guard;
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));
    REQUIRE(ith::detail::beginOpenFileDialog(window.id(), {}).hasValue());

    SUBCASE("several files") {
        const std::string longPath = "C:\\" + std::string(300, 'b') + ".fbx";
        const std::array<const char*, 4> files{"C:\\Art\\rock.gltf", "C:\\Art\\\xC3\xA9t\xC3\xA9.png", longPath.c_str(),
                                               nullptr};
        ith::detail::finishOpenFileDialog(files.data());

        const std::uint64_t before = platform.pumpTimeNs();
        const std::vector<Pumped> events = pump(platform);
        REQUIRE(events.size() == 4);
        CHECK(events[0].event.type == ith::EventType::FileDialogFile);
        CHECK(events[0].text == "C:\\Art\\rock.gltf");
        CHECK(events[1].event.type == ith::EventType::FileDialogFile);
        CHECK(events[1].text == "C:\\Art\\\xC3\xA9t\xC3\xA9.png");
        CHECK(events[2].event.type == ith::EventType::FileDialogFile);
        CHECK(events[2].text == longPath);
        CHECK(events[3].event.type == ith::EventType::FileDialogClosed);
        CHECK(events[3].text.empty());
        for (const Pumped& pumped : events) {
            CHECK(pumped.event.window == window.id());
            CHECK(pumped.event.timestampNs > before);
            CHECK(pumped.event.timestampNs <= platform.pumpTimeNs());
        }
    }
    SUBCASE("cancelled") {
        const std::array<const char*, 1> none{nullptr};
        ith::detail::finishOpenFileDialog(none.data());
        const std::vector<Pumped> events = pump(platform);
        REQUIRE(events.size() == 1);
        CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
        CHECK(events[0].text.empty());
    }
    SUBCASE("cancelled in zenity, which reports one empty name") {
        const std::array<const char*, 2> empty{"", nullptr};
        ith::detail::finishOpenFileDialog(empty.data());
        const std::vector<Pumped> events = pump(platform);
        REQUIRE(events.size() == 1);
        CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
        CHECK(events[0].text.empty());
    }
    SUBCASE("an error, with the library's message") {
        SDL_SetError("no file dialog is available");
        ith::detail::finishOpenFileDialog(nullptr);
        const std::vector<Pumped> events = pump(platform);
        REQUIRE(events.size() == 1);
        CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
        CHECK(events[0].text == "no file dialog is available");
    }
    SUBCASE("an error without a message still has one") {
        SDL_ClearError();
        ith::detail::finishOpenFileDialog(nullptr);
        const std::vector<Pumped> events = pump(platform);
        REQUIRE(events.size() == 1);
        CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
        CHECK_FALSE(events[0].text.empty());
    }
    // Every result is delivered once, and a callback without an open dialog is ignored.
    CHECK(pump(platform).empty());
    const std::array<const char*, 2> stray{"/stray.png", nullptr};
    ith::detail::finishOpenFileDialog(stray.data());
    CHECK(pump(platform).empty());
}

TEST_CASE("A file dialog result from another thread ends waitEvents") {
    const DialogGuard guard;
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    while (!platform.waitEvents(20ms).empty()) {}
    REQUIRE(ith::detail::beginOpenFileDialog(window.id(), {}).hasValue());

    std::thread finisher([] {
        std::this_thread::sleep_for(100ms);
        const std::array<const char*, 2> files{"/home/user/a.png", nullptr};
        ith::detail::finishOpenFileDialog(files.data());
        // SDL frees the thread-local data of threads it didn't create only through SDL_CleanupTLS.
        SDL_CleanupTLS();
    });
    // While the dialog is open each wait ends after 50 ms at most; the result ends one of them.
    const auto start = std::chrono::steady_clock::now();
    std::vector<Pumped> events;
    while (events.empty() && std::chrono::steady_clock::now() - start < 5s) {
        for (const ith::Event& event : platform.waitEvents(10s)) {
            events.push_back({.event = event, .text = std::string(event.text)});
        }
    }
    finisher.join();

    REQUIRE(events.size() == 2);
    CHECK(events[0].event.type == ith::EventType::FileDialogFile);
    CHECK(events[0].text == "/home/user/a.png");
    CHECK(events[1].event.type == ith::EventType::FileDialogClosed);
}

TEST_CASE("waitEvents waits at most 50 ms while a file dialog is open, and its full timeout after") {
    const DialogGuard guard;
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    while (!platform.waitEvents(20ms).empty()) {}
    REQUIRE(ith::detail::beginOpenFileDialog(window.id(), {}).hasValue());

    const auto start = std::chrono::steady_clock::now();
    CHECK(platform.waitEvents(10s).empty());
    // 50 ms, with room for a slow machine, far below the 10 s asked for.
    CHECK(std::chrono::steady_clock::now() - start < 2s);

    const std::array<const char*, 1> none{nullptr};
    ith::detail::finishOpenFileDialog(none.data());
    REQUIRE(platform.waitEvents(10s).size() == 1);
    // Closed: a wait lasts its timeout again.
    const auto again = std::chrono::steady_clock::now();
    CHECK(platform.waitEvents(200ms).empty());
    CHECK(std::chrono::steady_clock::now() - again >= 150ms);
}

TEST_CASE("showOpenFileDialog allows one dialog at a time and reports errors through FileDialogClosed") {
    const DialogGuard guard;
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));

    SUBCASE("busy until the result is pumped") {
        REQUIRE(platform.showOpenFileDialog(window, {.filters = kInvalidFilters, .allowMany = true}).hasValue());
        // SDL has already called back with its error, but the dialog is busy until a pump delivers
        // it.
        const ith::Expected<void> second = platform.showOpenFileDialog(window, {.filters = kInvalidFilters});
        REQUIRE_FALSE(second.hasValue());
        CHECK(second.error().code == "platform.dialog_busy");

        const std::vector<Pumped> events = pump(platform);
        REQUIRE(events.size() == 1);
        CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
        CHECK(events[0].event.window == window.id());
        CHECK(events[0].text.find("Invalid dialog file filters") != std::string::npos);

        // Free again once delivered; the default folder doesn't change the result.
        REQUIRE(
            platform.showOpenFileDialog(window, {.filters = kInvalidFilters, .defaultFolder = "C:/Art"}).hasValue());
        const std::vector<Pumped> again = pump(platform);
        REQUIRE(again.size() == 1);
        CHECK(again[0].event.type == ith::EventType::FileDialogClosed);
        CHECK_FALSE(again[0].text.empty());
    }
    SUBCASE("busy while the dialog is open") {
        REQUIRE(ith::detail::beginOpenFileDialog(window.id(), {}).hasValue());
        const ith::Expected<void> second = platform.showOpenFileDialog(window, {.filters = kInvalidFilters});
        REQUIRE_FALSE(second.hasValue());
        CHECK(second.error().code == "platform.dialog_busy");
        const std::array<const char*, 1> none{nullptr};
        ith::detail::finishOpenFileDialog(none.data());
        REQUIRE(pump(platform).size() == 1);
    }
}

TEST_CASE("A file dialog result not yet delivered is dropped with its Platform") {
    const DialogGuard guard;
    {
        ith::Platform platform = createHeadless();
        const ith::Window window = createWindow(platform);
        REQUIRE(ith::detail::beginOpenFileDialog(window.id(), {}).hasValue());
        const std::array<const char*, 2> files{"/a.png", nullptr};
        ith::detail::finishOpenFileDialog(files.data());
    }
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    for (const Pumped& pumped : pump(platform)) {
        CHECK(pumped.event.type != ith::EventType::FileDialogFile);
        CHECK(pumped.event.type != ith::EventType::FileDialogClosed);
    }
    // The new Platform's dialog is not busy.
    REQUIRE(platform.showOpenFileDialog(window, {.filters = kInvalidFilters}).hasValue());
    const std::vector<Pumped> events = pump(platform);
    REQUIRE(events.size() == 1);
    CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
    CHECK(events[0].event.window == window.id());
}

TEST_CASE("A file dialog open when its Platform goes delivers nothing and keeps the next one busy until it ends") {
    const DialogGuard guard;
    {
        ith::Platform platform = createHeadless();
        const ith::Window window = createWindow(platform);
        REQUIRE(ith::detail::beginOpenFileDialog(window.id(), {}).hasValue());
    }
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));
    const ith::Expected<void> busy = platform.showOpenFileDialog(window, {.filters = kInvalidFilters});
    REQUIRE_FALSE(busy.hasValue());
    CHECK(busy.error().code == "platform.dialog_busy");

    // The old dialog's callback comes late: its result is dropped.
    const std::array<const char*, 2> files{"/late.png", nullptr};
    ith::detail::finishOpenFileDialog(files.data());
    CHECK(pump(platform).empty());

    REQUIRE(platform.showOpenFileDialog(window, {.filters = kInvalidFilters}).hasValue());
    const std::vector<Pumped> events = pump(platform);
    REQUIRE(events.size() == 1);
    CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
    CHECK(events[0].event.window == window.id());
}

#ifdef ITH_ENABLE_ASSERTS
TEST_CASE("showOpenFileDialog asserts its preconditions") {
    const DialogGuard guard;
    ith::Platform platform = createHeadless();
    ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));

    SUBCASE("strings have no NUL character") {
        const std::array<ith::FileDialogFilter, 1> filters{
            {{.name = std::string_view("A\0B", 3), .patterns = kInvalidFilters[0].patterns}}};
        const ith::test::AssertCapture capture;
        static_cast<void>(platform.showOpenFileDialog(window, {.filters = filters}));
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() == "file dialog strings must not contain NUL characters");
    }
    SUBCASE("every filter has a pattern") {
        const std::array<ith::FileDialogFilter, 1> filters{{{.name = "Nothing", .patterns = ""}}};
        const ith::test::AssertCapture capture;
        REQUIRE(platform.showOpenFileDialog(window, {.filters = filters}).hasValue());
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() == "every file dialog filter needs a pattern");
        // When the assert continues, the dialog reports the error instead of showing.
        const std::vector<Pumped> events = pump(platform);
        REQUIRE(events.size() == 1);
        CHECK(events[0].event.type == ith::EventType::FileDialogClosed);
        CHECK(events[0].text == "a file dialog filter has an empty pattern");
    }
    SUBCASE("the window is open") {
        const ith::Window other = std::move(window);
        const ith::test::AssertCapture capture;
        // NOLINTNEXTLINE(bugprone-use-after-move): tested on purpose
        static_cast<void>(platform.showOpenFileDialog(window, {.filters = kInvalidFilters}));
        CHECK(capture.count() >= 1);
        CHECK(capture.lastMessage() == "the file dialog needs an open window");
    }
    SUBCASE("called on the main thread") {
        const ith::test::AssertCapture capture;
        // SDL is called off the main thread here only because the main thread is blocked in join()
        // meanwhile, so nothing runs concurrently.
        // SDL frees the thread-local data of threads it didn't create (here its error buffer) only
        // through SDL_CleanupTLS.
        std::thread([&] {
            static_cast<void>(platform.showOpenFileDialog(window, {.filters = kInvalidFilters}));
            SDL_CleanupTLS();
        }).join();
        CHECK(capture.count() >= 1);
        CHECK(capture.lastMessage().find("thread that created the Platform") != std::string::npos);
    }
    // The invalid filter made SDL call back at once; take the result so the dialog is free again.
    static_cast<void>(pump(platform));
}
#endif
