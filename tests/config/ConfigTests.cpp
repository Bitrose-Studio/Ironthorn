// Layered configuration (ADR-0035 rules 11 to 21): the merge patch, layers, skipped files, typed
// sections with defaults and warnings, minimal-difference writes, whole files, generations and
// reload, and the files the engine ships.

#include "ith/config/Config.h"

#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Uuid.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/TextArchive.h"

#include "AssertCapture.h"
#include "ConfigTypes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = ith::fs;
using ith::serialization::Value;
using ith::test::config::Preferences;

namespace {

constexpr ith::ConfigSection<Preferences> kPreferences{"Editor", "preferences"};

void removeTree(const std::string& path) {
    const ith::Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path);
    if (!info || !info->has_value()) {
        return;
    }
    if ((*info)->kind == fs::FileKind::Directory) {
        if (ith::Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(path)) {
            for (const fs::DirectoryEntry& entry : *entries) {
                removeTree(fs::joinPath(path, entry.name));
            }
        }
    }
    static_cast<void>(fs::remove(path));
}

// Three layer directories, engine, project and user, in a temporary folder.
class Layers {
public:
    Layers() {
        std::error_code error;
        const std::u8string base = std::filesystem::temp_directory_path(error).generic_u8string();
        REQUIRE_FALSE(error);
        m_root =
            fs::joinPath(std::string(base.begin(), base.end()), "ith-config-test-" + ith::Uuid::generate().toString());
        for (const char* layer : {"engine", "project", "user"}) {
            REQUIRE(fs::createDirectories(directory(layer)).hasValue());
        }
    }

    Layers(const Layers&) = delete;
    Layers& operator=(const Layers&) = delete;
    Layers(Layers&&) = delete;
    Layers& operator=(Layers&&) = delete;

    ~Layers() { removeTree(m_root); }

    [[nodiscard]] std::string directory(std::string_view layer) const { return fs::joinPath(m_root, layer); }

    [[nodiscard]] std::string file(std::string_view layer, std::string_view relative) const {
        return fs::joinPath(directory(layer), relative);
    }

    // Writes a settings file whose "data" is `data` (JSON text).
    void writeSettings(std::string_view layer, std::string_view name, std::string_view data) const {
        write(layer, std::string(name) + ".json",
              R"({"format": "ith.config", "version": 1, "data": )" + std::string(data) + "}");
    }

    void write(std::string_view layer, std::string_view relative, std::string_view text) const {
        REQUIRE(fs::writeFileAtomic(file(layer, relative), text).hasValue());
    }

    [[nodiscard]] std::string read(std::string_view layer, std::string_view relative) const {
        const ith::Expected<std::string> text = fs::readFile(file(layer, relative), std::size_t{1} << 20);
        return text ? *text : "<" + text.error().code + ">";
    }

    // The "data" of a settings file as compact JSON-like text for comparisons, or "<missing>".
    [[nodiscard]] Value data(std::string_view layer, std::string_view name) const {
        const std::string text = read(layer, std::string(name) + ".json");
        ith::Expected<Value, ith::serialization::Error> data =
            ith::serialization::readDocument(text, ith::kConfigFormat);
        return data ? *data : Value::string("<missing>");
    }

    [[nodiscard]] ith::Config config() const {
        return ith::Config({{.name = "engine", .directory = directory("engine")},
                            {.name = "project", .directory = directory("project"), .writable = true},
                            {.name = "user", .directory = directory("user"), .writable = true}});
    }

private:
    std::string m_root;
};

Value json(std::string_view text) {
    ith::Expected<Value, ith::serialization::Error> value = ith::serialization::parseJson(text);
    REQUIRE(value.hasValue());
    return value ? *value : Value();
}

std::vector<std::string> codesOf(const std::vector<ith::Error>& warnings) {
    std::vector<std::string> codes;
    for (const ith::Error& warning : warnings) {
        codes.push_back(warning.code);
    }
    return codes;
}

} // namespace

TEST_SUITE("config.merge_patch") {
    // The examples of RFC 7396, appendix A.
    TEST_CASE("the RFC 7396 examples merge as the RFC says") {
        struct Example {
            const char* target;
            const char* patch;
            const char* result;
        };

        const Example examples[] = {
            {R"({"a":"b"})", R"({"a":"c"})", R"({"a":"c"})"},
            {R"({"a":"b"})", R"({"b":"c"})", R"({"a":"b","b":"c"})"},
            {R"({"a":"b"})", R"({"a":null})", R"({})"},
            {R"({"a":"b","b":"c"})", R"({"a":null})", R"({"b":"c"})"},
            {R"({"a":["b"]})", R"({"a":"c"})", R"({"a":"c"})"},
            {R"({"a":"c"})", R"({"a":["b"]})", R"({"a":["b"]})"},
            {R"({"a":{"b":"c"}})", R"({"a":{"b":"d","c":null}})", R"({"a":{"b":"d"}})"},
            {R"({"a":[{"b":"c"}]})", R"({"a":[1]})", R"({"a":[1]})"},
            {R"(["a","b"])", R"(["c","d"])", R"(["c","d"])"},
            {R"({"a":"b"})", R"(["c"])", R"(["c"])"},
            {R"({"a":"foo"})", "null", "null"},
            {R"({"a":"foo"})", R"("bar")", R"("bar")"},
            {R"({"e":null})", R"({"a":1})", R"({"e":null,"a":1})"},
            {R"([1,2])", R"({"a":"b","c":null})", R"({"a":"b"})"},
            {R"({})", R"({"a":{"bb":{"ccc":null}}})", R"({"a":{"bb":{}}})"},
        };
        for (const Example& example : examples) {
            CAPTURE(example.target);
            CAPTURE(example.patch);
            Value target = json(example.target);
            ith::applyMergePatch(target, json(example.patch));
            CHECK(target == json(example.result));
        }
    }
}

TEST_SUITE("config.layers") {
    TEST_CASE("files merge from the lowest layer to the highest") {
        const Layers layers;
        layers.writeSettings("engine", "Editor",
                             R"({"preferences": {"idleFramesPerSecond": 30, "recent": ["a", "b"], "theme": "light"},
                                 "other": {"x": 1, "y": {"z": 2}}})");
        layers.writeSettings("project", "Editor",
                             R"({"preferences": {"recent": ["c"], "theme": null}, "other": {"y": {"w": 3}}})");
        // Comments are allowed in settings files.
        layers.write("user", "Editor.json", R"(// the user's settings
            {"format": "ith.config", "version": 1, "data": {"preferences": {"idleFramesPerSecond": 60 /* fast */}}})");
        ith::Config config = layers.config();
        CHECK(config.data("Editor") == json(R"({"preferences": {"idleFramesPerSecond": 60, "recent": ["c"]},
                       "other": {"x": 1, "y": {"z": 2, "w": 3}}})"));
        const Preferences preferences = config.get(kPreferences);
        CHECK(preferences.idleFramesPerSecond == 60);
        CHECK(preferences.recent == std::vector<std::string>{"c"});
        CHECK(preferences.theme == "dark"); // removed by null: the struct's default
        CHECK(preferences.unfocusedFramesPerSecond == 2);
        CHECK(config.warnings().empty());
        CHECK(config.data("Missing") == Value::object());
    }

    TEST_CASE("a broken file is skipped with one warning and the other layers still apply") {
        const Layers layers;
        layers.writeSettings("engine", "Editor", R"({"preferences": {"idleFramesPerSecond": 30}})");
        layers.write("project", "Editor.json", R"({"format": "ith.config", "version": 1, "data": {"preferences": )");
        layers.writeSettings("user", "Editor", R"({"preferences": {"theme": "light"}})");
        ith::Config config = layers.config();
        const Preferences preferences = config.get(kPreferences);
        CHECK(preferences.idleFramesPerSecond == 30);
        CHECK(preferences.theme == "light");
        static_cast<void>(config.get(kPreferences));
        CHECK(codesOf(config.warnings()) == std::vector<std::string>{"config.file_skipped"});
        CHECK(config.warnings().front().message.find("project") != std::string::npos);
    }

    TEST_CASE("duplicate keys, another format, a non-object and an oversized file are skipped") {
        const Layers layers;
        layers.writeSettings("engine", "A", R"({"s": {"x": 1, "x": 2}})");
        layers.write("engine", "B.json", R"({"format": "ith.editor_layout", "version": 1, "data": {}})");
        layers.writeSettings("engine", "C", R"([1, 2])");
        layers.writeSettings("engine", "D",
                             R"({"s": {"text": ")" + std::string(ith::kMaxConfigFileSize, 'x') + R"("}})");
        ith::Config config = layers.config();
        for (const char* file : {"A", "B", "C", "D"}) {
            CAPTURE(file);
            CHECK(config.data(file) == Value::object());
        }
        CHECK(codesOf(config.warnings()) == std::vector<std::string>(4, "config.file_skipped"));
        CHECK(config.warnings().front().message.find("'x' appears twice") != std::string::npos);
    }

    TEST_CASE("missing fields keep their defaults, unknown fields and wrong types warn once") {
        const Layers layers;
        layers.writeSettings("engine", "Editor",
                             R"({"preferences": {"idleFramesPerSecond": "fast", "colour": "red", "theme": "light",
                                                 "backgroundFramesPerSecond": 1, "window": {"width": 800, "depth": 3}}})");
        ith::Config config = layers.config();
        const Preferences preferences = config.get(kPreferences);
        CHECK(preferences.idleFramesPerSecond == 10);
        CHECK(preferences.theme == "light");
        CHECK(preferences.unfocusedFramesPerSecond == 1); // read through its former name
        CHECK(preferences.window.width == 800);
        CHECK(preferences.window.height == 720);
        static_cast<void>(config.get(kPreferences));
        std::vector<std::string> codes = codesOf(config.warnings());
        std::ranges::sort(codes);
        CHECK(codes ==
              std::vector<std::string>{"config.unknown_field", "config.wrong_type", "serialization.unknown_field"});
        const std::vector<ith::Error> taken = config.takeWarnings();
        CHECK(taken.size() == 3);
        CHECK(config.warnings().empty());
        static_cast<void>(config.get(kPreferences));
        CHECK(config.warnings().empty()); // the same warnings don't come back until reload
        config.reload();
        static_cast<void>(config.get(kPreferences));
        CHECK(config.warnings().size() == 3);
    }

    TEST_CASE("a section that is not an object gives the defaults with a warning") {
        const Layers layers;
        layers.writeSettings("engine", "Editor", R"({"preferences": 5})");
        ith::Config config = layers.config();
        CHECK(config.get(kPreferences).idleFramesPerSecond == 10);
        CHECK(codesOf(config.warnings()) == std::vector<std::string>{"config.wrong_type"});
    }
}

TEST_SUITE("config.set") {
    TEST_CASE("set writes only what differs from the layers below") {
        const Layers layers;
        layers.writeSettings("engine", "Editor", R"({"preferences": {"idleFramesPerSecond": 30}})");
        ith::Config config = layers.config();
        Preferences preferences = config.get(kPreferences);
        preferences.theme = "light";
        preferences.window.height = 900;
        preferences.recent = {"Maps/Harbour.ithmap"};
        REQUIRE(config.set(kPreferences, preferences).hasValue());
        CHECK(
            layers.data("user", "Editor") ==
            json(
                R"({"preferences": {"theme": "light", "recent": ["Maps/Harbour.ithmap"], "window": {"height": 900}}})"));
        CHECK(layers.read("user", "Editor.json").find("//") == std::string::npos);
        CHECK(config.get(kPreferences).window.height == 900);
        CHECK(config.get(kPreferences).idleFramesPerSecond == 30);

        // Back to what the layers below give: the fields go, and the file keeps an empty object.
        preferences = config.get(kPreferences);
        preferences.theme = "dark";
        preferences.window.height = 720;
        preferences.recent.clear();
        REQUIRE(config.set(kPreferences, preferences).hasValue());
        CHECK(layers.data("user", "Editor") == Value::object());

        // A value equal to a lower layer's is not written, so a later change there reaches the user.
        preferences.idleFramesPerSecond = 30;
        REQUIRE(config.set(kPreferences, preferences).hasValue());
        CHECK(layers.data("user", "Editor") == Value::object());
        layers.writeSettings("engine", "Editor", R"({"preferences": {"idleFramesPerSecond": 20}})");
        config.reload();
        CHECK(config.get(kPreferences).idleFramesPerSecond == 20);
        CHECK(config.warnings().empty());
    }

    TEST_CASE("set keeps key order and other keys, appends new ones and drops former names") {
        const Layers layers;
        layers.writeSettings("user", "Editor",
                             R"({"layouts": {"default": "Mine"},
                                 "preferences": {"theme": "light", "note": "kept", "backgroundFramesPerSecond": 1,
                                                 "idleFramesPerSecond": 5}})");
        ith::Config config = layers.config();
        Preferences preferences = config.get(kPreferences);
        CHECK(preferences.unfocusedFramesPerSecond == 1);
        preferences.idleFramesPerSecond = 6;
        preferences.limit = 120;
        REQUIRE(config.set(kPreferences, preferences).hasValue());
        const Value data = layers.data("user", "Editor");
        CHECK(data == json(R"({"layouts": {"default": "Mine"},
                               "preferences": {"theme": "light", "note": "kept", "idleFramesPerSecond": 6,
                                               "unfocusedFramesPerSecond": 1, "limit": 120}})"));
        // An empty optional can't override a lower value (a merge patch has no null value).
        preferences.limit.reset();
        REQUIRE(config.set(kPreferences, preferences).hasValue());
        CHECK(layers.data("user", "Editor").find("preferences")->find("limit") == nullptr);
    }

    TEST_CASE("set into another layer compares with the layers below that one") {
        const Layers layers;
        layers.writeSettings("user", "Editor", R"({"preferences": {"theme": "light"}})");
        ith::Config config = layers.config();
        Preferences preferences;
        preferences.theme = "blue";
        REQUIRE(config.set(kPreferences, preferences, "project").hasValue());
        CHECK(layers.data("project", "Editor") == json(R"({"preferences": {"theme": "blue"}})"));
        CHECK(config.get(kPreferences).theme == "light"); // the user layer still wins
    }

    TEST_CASE("a failed write changes nothing") {
        const Layers layers;
        ith::Config config({{.name = "engine", .directory = layers.directory("engine")},
                            {.name = "user", .directory = layers.file("user", "blocked/dir"), .writable = true}});
        layers.write("user", "blocked", "a file where a folder should be");
        Preferences preferences;
        preferences.theme = "light";
        const std::uint64_t generation = config.generation();
        const ith::Expected<void> written = config.set(kPreferences, preferences);
        REQUIRE_FALSE(written.hasValue());
        if (!written) {
            CHECK(written.error().code.starts_with("fs."));
        }
        CHECK(config.get(kPreferences).theme == "dark");
        CHECK(config.generation() == generation);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("writing into a layer that isn't writable asserts") {
        const Layers layers;
        ith::Config config = layers.config();
        ith::test::AssertCapture capture;
        static_cast<void>(config.set(kPreferences, Preferences{}, "engine"));
        CHECK(capture.count() >= 1);
    }
#endif
}

TEST_SUITE("config.files") {
    TEST_CASE("whole files come from the highest layer that has them") {
        const Layers layers;
        layers.write("engine", "Layouts/Default.json", "engine default");
        layers.write("engine", "Layouts/Wide.json", "engine wide");
        layers.write("user", "Layouts/Default.json", "user default");
        layers.write("user", "Layouts/Mine.json", "user mine");
        layers.write("project", "Layouts/Sub/Deep.json", "not listed");
        ith::Config config = layers.config();

        const auto found = config.findFile("Layouts/Default.json");
        REQUIRE(found.hasValue());
        REQUIRE((found && found->has_value()));
        if (found && found->has_value()) {
            CHECK((*found)->layer == 2);
            CHECK(fs::readFile((*found)->path, 100).valueOr(std::string()) == "user default");
        }
        CHECK((config.findFile("Layouts/None.json").hasValue() && !config.findFile("Layouts/None.json")->has_value()));
        CHECK(config.findFile("../escape.json").error().code == "config.invalid_path");
        CHECK(config.findFile("/abs.json").error().code == "config.invalid_path");

        const auto listed = config.listFiles("Layouts");
        REQUIRE(listed.hasValue());
        if (listed) {
            std::vector<std::string> names;
            std::vector<std::size_t> origins;
            for (const ith::ConfigFile& file : *listed) {
                names.push_back(file.name);
                origins.push_back(file.layer);
            }
            CHECK(names == std::vector<std::string>{"Default.json", "Mine.json", "Wide.json"});
            CHECK(origins == std::vector<std::size_t>{2, 2, 0});
        }
        CHECK(config.listFiles("Nowhere").valueOr(std::vector<ith::ConfigFile>{}).empty());
        CHECK(config.listFiles("..").error().code == "config.invalid_path");
    }

    TEST_CASE("writeFile writes into a layer and generations change") {
        const Layers layers;
        ith::Config config = layers.config();
        const std::uint64_t start = config.generation();
        CHECK(config.generation("Layouts/Mine.json") == 0);
        REQUIRE(config.writeFile("user", "Layouts/Mine.json", "mine").hasValue());
        CHECK(layers.read("user", "Layouts/Mine.json") == "mine");
        CHECK(config.generation() == start + 1);
        CHECK(config.generation("Layouts/Mine.json") == 1);
        CHECK(config.generation("Editor") == 0);
        CHECK(config.writeFile("user", "../x.json", "x").error().code == "config.invalid_path");

        REQUIRE(config.set(kPreferences, Preferences{}).hasValue());
        CHECK(config.generation() == start + 2);
        CHECK(config.generation("Editor") == 1);
        config.reload();
        CHECK(config.generation() == start + 3);
        CHECK(config.generation("Editor") == 2);
        CHECK(config.generation("Layouts/Mine.json") == 2);
        CHECK(config.generation("Other") == 1);
    }

    TEST_CASE("removeFile removes a whole file from a layer, and data below a layer merges the lower ones") {
        const Layers layers;
        layers.write("engine", "Layouts/Mine.json", "engine");
        layers.write("user", "Layouts/Mine.json", "user");
        ith::Config config = layers.config();
        const std::uint64_t start = config.generation("Layouts/Mine.json");
        REQUIRE(config.removeFile("user", "Layouts/Mine.json").hasValue());
        CHECK(layers.read("user", "Layouts/Mine.json") == "<fs.not_found>");
        CHECK(config.generation("Layouts/Mine.json") == start + 1);
        const auto found = config.findFile("Layouts/Mine.json");
        REQUIRE((found && found->has_value()));
        CHECK((found && found->has_value() && (*found)->layer == 0));
        CHECK(config.removeFile("user", "Layouts/Mine.json").error().code == "fs.not_found");
        CHECK(config.removeFile("user", "../x.json").error().code == "config.invalid_path");

        layers.writeSettings("engine", "Editor", R"({"layouts": {"defaultName": "Default", "x": 1}})");
        layers.writeSettings("project", "Editor", R"({"layouts": {"defaultName": "Studio"}})");
        layers.writeSettings("user", "Editor", R"({"layouts": {"defaultName": "Mine"}})");
        ith::Config merged = layers.config();
        CHECK(merged.data("Editor", 3) == merged.data("Editor"));
        CHECK(merged.data("Editor", 2) == json(R"({"layouts": {"defaultName": "Studio", "x": 1}})"));
        CHECK(merged.data("Editor", 1) == json(R"({"layouts": {"defaultName": "Default", "x": 1}})"));
        CHECK(merged.data("Editor", 0) == Value::object());
    }

    TEST_CASE("reload reads changed files again") {
        const Layers layers;
        layers.writeSettings("user", "Editor", R"({"preferences": {"theme": "light"}})");
        ith::Config config = layers.config();
        CHECK(config.get(kPreferences).theme == "light");
        layers.writeSettings("user", "Editor", R"({"preferences": {"theme": "blue"}})");
        CHECK(config.get(kPreferences).theme == "light");
        config.reload();
        CHECK(config.get(kPreferences).theme == "blue");
    }
}

TEST_SUITE("config.engine_files") {
    // The tests run in the repository root; the build copies engine/config next to executables.
    TEST_CASE("the shipped engine layer reads without warnings, also from next to the executable") {
        const ith::Expected<std::string> executable = fs::executableDirectory();
        REQUIRE(executable.hasValue());
        for (const std::string& directory :
             {std::string("engine/config"), fs::joinPath(executable.valueOr(""), "Config")}) {
            CAPTURE(directory);
            ith::Config config({{.name = "engine", .directory = directory}});
            const Value& keymap = config.data("Keymap");
            const Value* commands = keymap.find("commands");
            REQUIRE(commands != nullptr);
            if (commands != nullptr) {
                REQUIRE(commands->kind() == Value::Kind::Object);
                CHECK(commands->members().size() >= 10);
                for (const Value::Member& command : commands->members()) {
                    CAPTURE(command.key);
                    CHECK(command.key.starts_with("ith."));
                    REQUIRE(command.value.kind() == Value::Kind::Array);
                    for (const Value& chord : command.value.elements()) {
                        CHECK(chord.kind() == Value::Kind::String);
                    }
                }
                const Value* focus = commands->find("ith.viewport.focus_selection");
                CHECK((focus != nullptr && *focus == json(R"(["Keyboard.F"])")));
            }
            CHECK(config.data("Editor").kind() == Value::Kind::Object);
            CHECK(config.warnings().empty());

            // The Default layout is an ith.editor_layout document; editor_core's layout tests check its
            // shape (ADR-0040 rule 18).
            const auto layout = config.findFile("Layouts/Default.json");
            REQUIRE((layout && layout->has_value()));
            if (layout && layout->has_value()) {
                constexpr ith::serialization::DocumentFormat kLayoutFormat{
                    .name = "ith.editor_layout", .version = 1, .minimumVersion = 1};
                const ith::Expected<Value, ith::serialization::Error> document =
                    ith::serialization::readDocument(fs::readFile((*layout)->path, 1 << 20).valueOr(""), kLayoutFormat);
                REQUIRE(document.hasValue());
                CHECK((document && document->find("root") != nullptr));
            }
        }
    }
}
