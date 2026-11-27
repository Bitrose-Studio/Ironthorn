// The Outliner and Details panels (ADR-0008, ADR-0040 rule 3), headless on the RHI's null device,
// driven by mouse and key events through the platform backend and the keymap consumer: a click
// selects, F2 renames in place, a drag reparents, a Details slider drag is one undo entry with a
// live preview, Esc cancels it, a drag that can't begin an edit changes nothing, a click types a
// number into a field (a rotation in degrees, several entities at once), and one drag edits every
// selected entity. Each panel is drawn in a fixed window so the tests know where its rows are.

#include "ith/config/Config.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_ui/DetailsPanel.h"
#include "ith/editor_ui/OutlinerPanel.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/math/Math.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"

#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)

using namespace ith;
using namespace ith::test::ui;

namespace {

const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        return result;
    }();
    return kTypes;
}

Uuid idOf(std::uint64_t n) {
    return Uuid(0xED, n);
}

Keymap engineKeymap(const EditorSession& session) {
    const Config config({{.name = "engine", .directory = "engine/config"}});
    std::vector<Error> warnings;
    return Keymap::load(config, session.commands(), warnings);
}

constexpr std::string_view kTranslation = "ith::LocalTransform.value.translation";

// A session with the Outliner and Details panels, each drawn in a fixed window: the Outliner on the
// left, Details beside it. The engine keymap's consumer sits under the UI, as in the editor.
struct PanelFixture {
    PanelFixture() : session(types()), keymap(engineKeymap(session)), shortcuts(session, keymap), ui({}, &shortcuts) {
        registerOutliner(session, outliner);
        registerDetails(session.panels(), details);
    }

    void draw() {
        // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
        constexpr ImGuiWindowFlags kFixed =
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 700.0f));
        if (ImGui::Begin("Outliner###test.outliner", nullptr, kFixed)) {
            session.panels().find("ith.outliner")->draw("ith.outliner");
        }
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(400.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(880.0f, 700.0f));
        if (ImGui::Begin("Details###test.details", nullptr, kFixed)) {
            session.panels().find("ith.details")->draw("ith.details");
        }
        ImGui::End();
    }

    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            ui.frame([this] { draw(); });
        }
    }

    // One frame for each event, so ImGui sees the mouse go down, move and up on separate frames.
    void step(const Event& event) {
        ui.events({event});
        frames(1);
    }

    void move(float x, float y) { step({.type = EventType::MouseMoved, .x = x, .y = y}); }

    void press(float x, float y) {
        move(x, y);
        step({.type = EventType::MouseButtonDown, .mouseButton = MouseButton::Left, .clicks = 1, .x = x, .y = y});
    }

    void release(float x, float y) {
        step({.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = x, .y = y});
    }

    void click(float x, float y) {
        press(x, y);
        release(x, y);
        frames(1);
    }

    // Moves the held mouse from (x0, y0) to (x1, y1) over `count` frames.
    void dragTo(float x0, float y0, float x1, float y1, int count) {
        for (int i = 1; i <= count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(count);
            move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
        }
    }

    void key(Key pressed) {
        step({.type = EventType::KeyDown, .key = pressed});
        step({.type = EventType::KeyUp, .key = pressed});
    }

    void create(std::uint64_t n, std::string_view label) {
        const std::string args = R"({"id": ")" + idOf(n).toString() + R"(", "label": ")" + std::string(label) + "\"}";
        auto parsed = serialization::parseJson(args);
        REQUIRE(parsed.hasValue());
        REQUIRE(session.execute("ith.entity.create", *parsed).hasValue());
    }

    void select(std::vector<std::uint64_t> ids) {
        serialization::Value list = serialization::Value::array();
        for (const std::uint64_t n : ids) {
            list.elements().push_back(serialization::Value::string(idOf(n).toString()));
        }
        serialization::Value args = serialization::Value::object();
        args.set("entities", std::move(list));
        REQUIRE(session.execute("ith.selection.set", args).hasValue());
    }

    UiRect row(std::uint64_t n) const {
        const std::optional<UiRect> rect = outliner.rowRect(idOf(n));
        REQUIRE(rect.has_value());
        return *rect;
    }

    UiRect value(std::string_view key) const {
        const std::optional<UiRect> rect = details.valueRect(key);
        REQUIRE_MESSAGE(rect.has_value(), std::string(key));
        return *rect;
    }

    [[nodiscard]] const Transform& transformOf(std::uint64_t n) const {
        const World& world = session.document().world();
        const auto* local = world.get<const LocalTransform>(world.findByStableId(idOf(n)));
        REQUIRE(local != nullptr);
        return local->value;
    }

    [[nodiscard]] std::string labelOf(std::uint64_t n) const {
        const World& world = session.document().world();
        return world.get<const EditorLabel>(world.findByStableId(idOf(n)))->name;
    }

    [[nodiscard]] std::size_t entries() const { return session.document().history().cursor(); }

    EditorSession session;
    Keymap keymap;
    KeymapConsumer shortcuts;
    UiFixture ui;
    OutlinerPanel outliner{session};
    DetailsPanel details{session};
};

} // namespace

TEST_SUITE("editor_ui.panels") {
    TEST_CASE("the panels register as Outliner and Details, docked at the right") {
        PanelFixture fixture;
        const PanelDesc* outliner = fixture.session.panels().find("ith.outliner");
        const PanelDesc* details = fixture.session.panels().find("ith.details");
        REQUIRE(outliner != nullptr);
        REQUIRE(details != nullptr);
        CHECK(outliner->displayName == "Outliner");
        CHECK(details->displayName == "Details");
        CHECK(outliner->defaultDock == DockPlace::Right);
        CHECK(details->defaultDock == DockPlace::Right);
        // An empty level draws both, with Details asking for a selection.
        fixture.frames(3);
        CHECK(fixture.outliner.emptyRect().has_value());
        CHECK_FALSE(fixture.details.valueRect(kTranslation).has_value());
    }

    TEST_CASE("a click in the Outliner selects; a click on the empty space clears the selection") {
        PanelFixture fixture;
        fixture.create(1, "Table");
        fixture.create(2, "Lamp");
        fixture.frames(3);
        const UiRect table = fixture.row(1);
        fixture.click(table.centerX(), table.centerY());
        CHECK(fixture.session.document().selection().entities() == std::vector<Uuid>{idOf(1)});
        // Details now shows the Table.
        fixture.frames(1);
        CHECK(fixture.details.valueRect(kTranslation).has_value());
        const UiRect empty = *fixture.outliner.emptyRect();
        fixture.click(empty.centerX(), empty.y + 10.0f);
        CHECK(fixture.session.document().selection().empty());
        CHECK(fixture.entries() == 2); // selecting makes no undo entry
    }

    TEST_CASE("F2 renames the selection in place: the typed name is kept on Enter, Esc drops it") {
        PanelFixture fixture;
        fixture.create(1, "Table");
        fixture.frames(3);
        fixture.key(Key::F2);
        fixture.frames(2);
        REQUIRE(fixture.outliner.model().renaming() == idOf(1));
        CHECK(fixture.ui.ui->wantsTextInput());
        // The field starts with the whole name selected, so typing replaces it.
        fixture.step({.type = EventType::TextInput, .text = "Desk"});
        fixture.key(Key::Enter);
        fixture.frames(2);
        CHECK_FALSE(fixture.outliner.model().renaming().has_value());
        CHECK(fixture.labelOf(1) == "Desk");
        CHECK(fixture.entries() == 2);

        fixture.key(Key::F2);
        fixture.frames(2);
        fixture.step({.type = EventType::TextInput, .text = "Chair"});
        fixture.key(Key::Escape);
        fixture.frames(2);
        CHECK_FALSE(fixture.outliner.model().renaming().has_value());
        CHECK(fixture.labelOf(1) == "Desk");
        CHECK(fixture.entries() == 2);
        // Esc in the field went to the field, not to Select None.
        CHECK(fixture.session.document().selection().entities() == std::vector<Uuid>{idOf(1)});
    }

    TEST_CASE("dragging a row onto another reparents it in one undo entry") {
        PanelFixture fixture;
        fixture.create(1, "Table");
        fixture.create(2, "Lamp");
        fixture.frames(3);
        const UiRect lamp = fixture.row(2);
        const UiRect table = fixture.row(1);
        const std::size_t before = fixture.entries();
        fixture.press(lamp.centerX(), lamp.centerY());
        fixture.dragTo(lamp.centerX(), lamp.centerY(), table.centerX(), table.centerY(), 8);
        fixture.frames(2);
        fixture.release(table.centerX(), table.centerY());
        fixture.frames(2);
        const World& world = fixture.session.document().world();
        const auto* link = world.get<const ChildOf>(world.findByStableId(idOf(2)));
        REQUIRE(link != nullptr);
        CHECK(link->parent == world.findByStableId(idOf(1)));
        CHECK(fixture.entries() == before + 1);
        REQUIRE(fixture.session.execute("ith.edit.undo").hasValue());
        CHECK_FALSE(world.has<ChildOf>(world.findByStableId(idOf(2))));
    }

    TEST_CASE("a Details slider drag previews live and becomes one undo entry; Esc cancels a drag") {
        PanelFixture fixture;
        fixture.create(1, "Table");
        fixture.frames(3);
        const UiRect translation = fixture.value(kTranslation);
        // The X field is the first third of the three.
        const float x = translation.x + translation.width / 6.0f;
        const float y = translation.centerY();
        const std::size_t before = fixture.entries();

        fixture.press(x, y);
        fixture.dragTo(x, y, x + 60.0f, y, 6);
        const float dragged = fixture.transformOf(1).translation.x;
        CHECK(dragged > 0.5f); // applied while the mouse is still down
        CHECK(fixture.session.document().transactions().isOpen());
        fixture.dragTo(x + 60.0f, y, x + 100.0f, y, 4);
        fixture.release(x + 100.0f, y);
        fixture.frames(2);
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        CHECK(fixture.transformOf(1).translation.x > dragged);
        CHECK(fixture.transformOf(1).translation.y == 0.0f);
        CHECK(fixture.entries() == before + 1);
        CHECK(fixture.session.document().history().entries().back().name == "Set Property");
        REQUIRE(fixture.session.execute("ith.edit.undo").hasValue());
        CHECK(fixture.transformOf(1).translation.x == 0.0f);

        // Esc during the drag puts the value back and records nothing. (Frames pass first, so the press
        // isn't a double click, which would type into the field.)
        fixture.frames(30);
        fixture.press(x, y);
        fixture.dragTo(x, y, x + 60.0f, y, 6);
        CHECK(fixture.transformOf(1).translation.x > 0.5f);
        fixture.key(Key::Escape);
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        CHECK(fixture.transformOf(1).translation.x == 0.0f);
        fixture.dragTo(x + 60.0f, y, x + 90.0f, y, 3); // the rest of the drag does nothing
        fixture.release(x + 90.0f, y);
        fixture.frames(2);
        CHECK(fixture.transformOf(1).translation.x == 0.0f);
        CHECK(fixture.entries() == before);
        // Esc reached the drag, not Select None.
        CHECK(fixture.session.document().selection().entities() == std::vector<Uuid>{idOf(1)});
    }

    TEST_CASE("a click on a Details number field types into it: Enter or a click away sets it, Esc drops it") {
        constexpr std::string_view kScale = "ith::LocalTransform.value.scale";
        PanelFixture fixture;
        fixture.create(1, "Cube");
        fixture.frames(3);
        const UiRect scale = fixture.value(kScale);
        const float x = scale.x + scale.width / 6.0f;
        const float z = scale.x + scale.width * 5.0f / 6.0f;
        const float y = scale.centerY();
        const std::size_t before = fixture.entries();

        // A click without moving opens the X field's text, all selected; "2" and Enter set it.
        fixture.click(x, y);
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.step({.type = EventType::TextInput, .text = "2"});
        fixture.key(Key::Enter);
        fixture.frames(2);
        CHECK_FALSE(fixture.ui.ui->wantsTextInput());
        CHECK(fixture.transformOf(1).scale == Vec3{2.0f, 1.0f, 1.0f});
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        CHECK(fixture.entries() == before + 1);
        CHECK(fixture.session.document().history().entries().back().name == "Set Property");

        // Z: "4", then a click on empty space keeps it.
        fixture.frames(30);
        fixture.click(z, y);
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.step({.type = EventType::TextInput, .text = "4"});
        fixture.click(scale.x, 650.0f);
        fixture.frames(2);
        CHECK_FALSE(fixture.ui.ui->wantsTextInput());
        CHECK(fixture.transformOf(1).scale == Vec3{2.0f, 1.0f, 4.0f});
        CHECK(fixture.entries() == before + 2);

        // Enter without typing records nothing.
        fixture.frames(30);
        fixture.click(z, y);
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.key(Key::Enter);
        fixture.frames(2);
        CHECK(fixture.entries() == before + 2);

        // Esc after typing drops the text; it reaches the field, not Select None.
        fixture.frames(30);
        fixture.click(z, y);
        fixture.frames(2);
        fixture.step({.type = EventType::TextInput, .text = "9"});
        fixture.key(Key::Escape);
        fixture.frames(2);
        CHECK_FALSE(fixture.ui.ui->wantsTextInput());
        CHECK(fixture.transformOf(1).scale.z == 4.0f);
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        CHECK(fixture.entries() == before + 2);
        CHECK(fixture.session.document().selection().entities() == std::vector<Uuid>{idOf(1)});

        REQUIRE(fixture.session.execute("ith.edit.undo").hasValue());
        CHECK(fixture.transformOf(1).scale == Vec3{2.0f, 1.0f, 1.0f});
    }

    TEST_CASE("Ctrl + click on a Details number field types into it too") {
        PanelFixture fixture;
        fixture.create(1, "Cube");
        fixture.frames(3);
        const UiRect scale = fixture.value("ith::LocalTransform.value.scale");
        const float x = scale.x + scale.width / 2.0f;
        const float y = scale.centerY();
        const std::size_t before = fixture.entries();
        fixture.move(x, y);
        fixture.step({.type = EventType::KeyDown, .key = Key::LeftControl, .modifiers = KeyModifiers::Control});
        fixture.step({.type = EventType::MouseButtonDown,
                      .modifiers = KeyModifiers::Control,
                      .mouseButton = MouseButton::Left,
                      .clicks = 1,
                      .x = x,
                      .y = y});
        fixture.step({.type = EventType::MouseButtonUp,
                      .modifiers = KeyModifiers::Control,
                      .mouseButton = MouseButton::Left,
                      .x = x,
                      .y = y});
        fixture.step({.type = EventType::KeyUp, .key = Key::LeftControl});
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.step({.type = EventType::TextInput, .text = "3"});
        fixture.key(Key::Enter);
        fixture.frames(2);
        CHECK(fixture.transformOf(1).scale == Vec3{1.0f, 3.0f, 1.0f});
        CHECK(fixture.entries() == before + 1);
    }

    TEST_CASE("degrees typed into the Rotation field set the rotation in one undo entry") {
        PanelFixture fixture;
        fixture.create(1, "Cube");
        fixture.frames(3);
        const UiRect rotation = fixture.value("ith::LocalTransform.value.rotation");
        const float x = rotation.x + rotation.width / 6.0f;
        const float y = rotation.centerY();
        const std::size_t before = fixture.entries();
        fixture.click(x, y);
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.step({.type = EventType::TextInput, .text = "90"});
        fixture.key(Key::Enter);
        fixture.frames(2);
        const Quat expected = glm::angleAxis(glm::radians(90.0f), Vec3{1.0f, 0.0f, 0.0f});
        CHECK(std::abs(glm::dot(fixture.transformOf(1).rotation, expected)) == doctest::Approx(1.0f));
        CHECK(fixture.entries() == before + 1);
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
    }

    TEST_CASE("a number typed into a field with several entities selected sets all of them in one undo entry") {
        PanelFixture fixture;
        fixture.create(1, "Table");
        fixture.create(2, "Lamp");
        fixture.select({1, 2});
        fixture.frames(3);
        const UiRect scale = fixture.value("ith::LocalTransform.value.scale");
        const float x = scale.x + scale.width / 6.0f;
        const float y = scale.centerY();
        const std::size_t before = fixture.entries();
        fixture.click(x, y);
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.step({.type = EventType::TextInput, .text = "2"});
        fixture.key(Key::Enter);
        fixture.frames(2);
        CHECK(fixture.transformOf(1).scale == Vec3{2.0f, 1.0f, 1.0f});
        CHECK(fixture.transformOf(2).scale == Vec3{2.0f, 1.0f, 1.0f});
        CHECK(fixture.entries() == before + 1);
        REQUIRE(fixture.session.execute("ith.edit.undo").hasValue());
        CHECK(fixture.transformOf(1).scale.x == 1.0f);
        CHECK(fixture.transformOf(2).scale.x == 1.0f);
    }

    TEST_CASE("a Details drag that can't begin an edit (while playing) changes nothing and warns once") {
        PanelFixture fixture;
        fixture.create(1, "Table");
        fixture.frames(3);
        const UiRect translation = fixture.value(kTranslation);
        const float x = translation.x + translation.width / 6.0f;
        const float y = translation.centerY();
        const std::size_t before = fixture.entries();
        fixture.session.document().setPlaying(true);
        LogRingBuffer log(64);
        REQUIRE(addLogSink(log.sink()).hasValue());
        fixture.press(x, y);
        fixture.dragTo(x, y, x + 80.0f, y, 8);
        fixture.release(x + 80.0f, y);
        fixture.frames(2);
        removeLogSink(log.sink());
        fixture.session.document().setPlaying(false);
        CHECK(fixture.transformOf(1).translation.x == 0.0f);
        CHECK(fixture.entries() == before);
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        std::vector<LogEntry> entries;
        static_cast<void>(log.read(0, entries));
        std::size_t warnings = 0;
        for (const LogEntry& entry : entries) {
            if (entry.level == LogLevel::Warning && std::string_view(entry.category->name()) == "Details") {
                ++warnings;
            }
        }
        CHECK(warnings == 1);
    }

    TEST_CASE("one Details drag edits every selected entity in one undo entry") {
        PanelFixture fixture;
        fixture.create(1, "Table");
        fixture.create(2, "Lamp");
        REQUIRE(fixture.session
                    .execute("ith.entity.set_property",
                             *serialization::parseJson(
                                 R"({"ids": [")" + idOf(2).toString() +
                                 R"("], "path": "ith::LocalTransform.value.translation.x", "value": 5})"))
                    .hasValue());
        fixture.select({1, 2});
        fixture.frames(3);
        // The two differ, so the field shows Multiple Values until the drag sets them.
        const std::optional<std::size_t> row = fixture.details.model().findRow(kTranslation);
        REQUIRE(row.has_value());
        CHECK(fixture.details.model().rows()[*row].mixed);
        const UiRect translation = fixture.value(kTranslation);
        const float x = translation.x + translation.width / 6.0f;
        const float y = translation.centerY();
        const std::size_t before = fixture.entries();
        fixture.press(x, y);
        fixture.dragTo(x, y, x + 80.0f, y, 8);
        fixture.release(x + 80.0f, y);
        fixture.frames(2);
        CHECK(fixture.entries() == before + 1);
        CHECK(fixture.transformOf(1).translation.x == fixture.transformOf(2).translation.x);
        CHECK(fixture.transformOf(1).translation.x != 0.0f);
        CHECK_FALSE(fixture.details.model().rows()[*fixture.details.model().findRow(kTranslation)].mixed);
        REQUIRE(fixture.session.execute("ith.edit.undo").hasValue());
        CHECK(fixture.transformOf(1).translation.x == 0.0f);
        CHECK(fixture.transformOf(2).translation.x == 5.0f);
    }
}
