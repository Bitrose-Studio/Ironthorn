// The translate gizmo of a viewport (ADR-0042 rules 24 to 26 and 30), headless, through a real Input
// with the keymap consumer below the viewport consumer, as the editor builds it: when the gizmo
// shows and where; a press on a handle drags it (never a click), previewed once per frame and one
// undo entry on the release; Esc cancels and restores; Alt at the press duplicates the selection and
// drags the copies; a press off the handles still clicks or orbits; snapping, the local gizmo space,
// parents that rotate and scale, and selected children of selected parents; GizmoDrag's refusals.

#include "ith/editor_core/ViewportGizmo.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorChanges.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Gizmo.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_core/ViewportInput.h"
#include "ith/editor_core/Viewports.h"
#include "ith/input/Input.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"
#include "ith/math/Types.h"
#include "ith/platform_events/Event.h"

#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using ith::Event;
using ith::EventType;
using ith::GizmoHandle;
using ith::Key;
using ith::KeyModifiers;
using ith::MouseButton;
using ith::Vec2;
using ith::Vec3;
using ith::ViewportGesture;
using ith::ViewportTool;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::run;
using ith::test::editor::types;

constexpr ith::WindowId kWindow = 1;
constexpr float kEpsilon = 1e-3f;

bool same(const Vec3& a, const Vec3& b, float epsilon = kEpsilon) {
    return glm::all(glm::epsilonEqual(a, b, epsilon));
}

std::string number(float value) {
    std::array<char, 32> text{};
    const std::to_chars_result written = std::to_chars(text.data(), text.data() + text.size(), value);
    return {text.data(), written.ptr};
}

std::string vec(const Vec3& v) {
    return R"({"x": )" + number(v.x) + R"(, "y": )" + number(v.y) + R"(, "z": )" + number(v.z) + "}";
}

Event press(float x, float y, KeyModifiers modifiers = KeyModifiers::None) {
    return {.type = EventType::MouseButtonDown,
            .modifiers = modifiers,
            .mouseButton = MouseButton::Left,
            .clicks = 1,
            .x = x,
            .y = y};
}

Event release(float x, float y) {
    return {.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = x, .y = y};
}

Event moved(float x, float y) {
    return {.type = EventType::MouseMoved, .x = x, .y = y};
}

Event key(Key code) {
    return {.type = EventType::KeyDown, .key = code};
}

// A quarter turn about +Y: local X along world -Z.
const ith::Quat kQuarterTurn = glm::angleAxis(ith::kHalfPi, ith::kWorldUp);

// An editor window's stack: the keymap consumer (no bindings: it only cancels the open transaction
// on Esc), and the viewport consumer with the gizmo above it, viewport 1's image at (100, 100) to
// (500, 400) with the default camera, looking at the origin. The tool is Translate, snapping on at
// 0.1 m.
struct Fixture {
    Fixture() {
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &viewports});
        ith::ViewportState& first = state();
        first.imagePosition = {100.0f, 100.0f};
        first.imageSize = {400.0f, 300.0f};
        first.imageHovered = true;
        events({Event{.type = EventType::WindowFocusGained}});
    }

    void events(std::vector<Event> list) {
        timeNs += 16'000'000;
        for (Event& event : list) {
            event.timestampNs = timeNs;
            event.window = kWindow;
        }
        input.processEvents(list, timeNs);
    }

    // Routes `list` as one frame and updates the consumer, as the editor's loop does.
    void frame(std::vector<Event> list) {
        events(std::move(list));
        static_cast<void>(viewports.update(1.0f / 60.0f));
    }

    ith::ViewportState& state() { return session.viewports().at(0); }

    // Creates entity n with a transform, a rotation, a scale and optionally a parent; it becomes the
    // selection.
    void create(std::uint64_t n, const Vec3& translation, std::optional<std::uint64_t> parent = std::nullopt,
                const ith::Quat& rotation = ith::Transform{}.rotation, const Vec3& scale = Vec3(1.0f)) {
        std::string args = R"({"id": ")" + idText(n) + R"(", "components": {"ith::LocalTransform": {"value": )" +
                           R"({"translation": )" + vec(translation) + R"(, "rotation": {"x": )" + number(rotation.x) +
                           R"(, "y": )" + number(rotation.y) + R"(, "z": )" + number(rotation.z) + R"(, "w": )" +
                           number(rotation.w) + R"(}, "scale": )" + vec(scale) + "}}}";
        if (parent.has_value()) {
            args += R"(, "parent": ")" + idText(*parent) + R"(")";
        }
        args += "}";
        run(session, "ith.entity.create", args);
    }

    void select(const std::vector<std::uint64_t>& ids) {
        std::string list;
        for (const std::uint64_t n : ids) {
            list += (list.empty() ? "\"" : ", \"") + idText(n) + "\"";
        }
        run(session, "ith.selection.set", R"({"entities": [)" + list + "]}");
    }

    [[nodiscard]] ith::Entity entity(std::uint64_t n) const {
        const ith::Entity found = session.document().world().findByStableId(idOf(n));
        REQUIRE_FALSE(found.isNull());
        return found;
    }

    [[nodiscard]] Vec3 translationOf(std::uint64_t n) const {
        return session.document().world().get<const ith::LocalTransform>(entity(n))->value.translation;
    }

    [[nodiscard]] Vec3 worldPositionOf(std::uint64_t n) const {
        return Vec3(session.document().world().worldMatrixOf(entity(n))[3]);
    }

    [[nodiscard]] ith::ViewportGizmo gizmo() {
        const std::optional<ith::ViewportGizmo> shown = ith::viewportGizmo(session, state(), 1.0f);
        REQUIRE(shown.has_value());
        return *shown;
    }

    // The window pixel in the middle of `handle`: an arrow's midpoint, a plane square's centre.
    [[nodiscard]] Vec2 pixelOf(GizmoHandle handle) {
        const ith::GizmoLayout layout = gizmo().layout;
        Vec2 pixel{0.0f};
        if (const std::optional<std::size_t> axis = ith::gizmoAxisOf(handle)) {
            pixel = 0.5f * (layout.center + layout.axisEnds[*axis]);
        } else if (const std::optional<std::size_t> plane = ith::gizmoPlaneOf(handle)) {
            const std::array<Vec2, 4>& corners = layout.planes[*plane];
            pixel = 0.25f * (corners[0] + corners[1] + corners[2] + corners[3]);
        }
        return state().imagePosition + pixel;
    }

    // The point a press at window pixel `press` grabs on `handle`.
    [[nodiscard]] Vec3 grabbed(GizmoHandle handle, Vec2 press) {
        const std::optional<ith::GizmoGrab> grab = ith::grabGizmo(
            handle, gizmo().frame, ith::pickRay(state().camera, state().imageSize, press - state().imagePosition));
        REQUIRE(grab.has_value());
        return grab->start;
    }

    // Where `point` shows, in window pixels.
    [[nodiscard]] Vec2 windowPixelOf(const Vec3& point) {
        const std::optional<Vec2> pixel = ith::projectToImage(state().camera, state().imageSize, point);
        REQUIRE(pixel.has_value());
        return state().imagePosition + *pixel;
    }

    // Presses on `handle`, then moves to where the grabbed point moved by `offset` shows, and updates;
    // returns that pixel. The drag stays held.
    Vec2 dragBy(GizmoHandle handle, const Vec3& offset, KeyModifiers modifiers = KeyModifiers::None) {
        const Vec2 start = pixelOf(handle);
        const Vec3 point = grabbed(handle, start);
        frame({press(start.x, start.y, modifiers)});
        REQUIRE(viewports.gesture() == ViewportGesture::Gizmo);
        const Vec2 target = windowPixelOf(point + offset);
        frame({moved(target.x, target.y)});
        return target;
    }

    [[nodiscard]] std::size_t undoEntries() const { return session.document().history().cursor(); }

    ith::EditorSession session{types()};
    ith::Keymap keymap;
    ith::KeymapConsumer shortcuts{session, keymap};
    ith::ViewportConsumer viewports{session};
    ith::Input input;
    std::uint64_t timeNs = 1'000'000'000;
};

} // namespace

TEST_SUITE("editor_core.viewport_gizmo") {
    TEST_CASE("the gizmo shows with the translate tool and a selection, at the primary's world position") {
        Fixture fixture;
        CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value()); // nothing selected

        fixture.create(1, Vec3{1.0f, 0.0f, 0.0f});
        fixture.create(2, Vec3{0.0f, 1.0f, 0.0f}, 1);
        CHECK(same(fixture.gizmo().frame.origin, Vec3{1.0f, 1.0f, 0.0f}));
        fixture.select({2, 1}); // the primary is the last
        CHECK(same(fixture.gizmo().frame.origin, Vec3{1.0f, 0.0f, 0.0f}));
        // At display scale 2 the arrows are 200 pixels long.
        const std::optional<ith::ViewportGizmo> two = ith::viewportGizmo(fixture.session, fixture.state(), 2.0f);
        REQUIRE(two.has_value());
        const std::optional<ith::GizmoLayout> expected = ith::layoutGizmo(
            fixture.state().camera, fixture.state().imageSize, two->frame, 2.0f * ith::kGizmoArrowLength);
        REQUIRE(expected.has_value());
        CHECK(two->layout.axisEnds == expected->axisEnds);

        SUBCASE("not with the rotate or scale tool") {
            fixture.session.viewports().setTool(ViewportTool::Rotate);
            CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value());
            fixture.session.viewports().setTool(ViewportTool::Scale);
            CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value());
        }
        SUBCASE("not in game view") {
            run(fixture.session, "ith.viewport.game_view");
            CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value());
        }
        SUBCASE("not while playing") {
            fixture.session.document().setPlaying(true);
            CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value());
            fixture.session.document().setPlaying(false);
        }
        SUBCASE("not on an image without area") {
            fixture.state().imageSize = Vec2{0.0f};
            CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value());
        }
        SUBCASE("not with an empty selection") {
            run(fixture.session, "ith.edit.select_none");
            CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value());
        }
    }

    TEST_CASE("in local space the axes follow the primary's rotation") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f}, std::nullopt, kQuarterTurn, Vec3{3.0f});
        CHECK(same(fixture.gizmo().frame.axes[0], ith::kWorldRight));
        run(fixture.session, "ith.viewport.toggle_gizmo_space");
        const ith::GizmoFrame frame = fixture.gizmo().frame;
        CHECK(same(frame.axes[0], Vec3{0.0f, 0.0f, -1.0f}));
        CHECK(same(frame.axes[2], ith::kWorldRight));

        // A drag along the local X arrow moves along world -Z.
        const Vec2 target = fixture.dragBy(GizmoHandle::X, Vec3{0.0f, 0.0f, -1.0f});
        CHECK(same(fixture.translationOf(1), Vec3{0.0f, 0.0f, -1.0f}));
        fixture.frame({release(target.x, target.y)});
    }

    TEST_CASE("a press on an arrow drags along it: previewed each frame, one undo entry on the release") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const std::size_t entries = fixture.undoEntries();
        const Vec2 start = fixture.pixelOf(GizmoHandle::X);
        const Vec3 point = fixture.grabbed(GizmoHandle::X, start);
        fixture.events({press(start.x, start.y)});
        CHECK(fixture.viewports.holdsDrag());
        CHECK(fixture.viewports.gesture() == ViewportGesture::Gizmo);
        CHECK(fixture.state().gizmoDrag == GizmoHandle::X);
        CHECK(fixture.session.document().transactions().isOpen());

        // The motion is previewed by update(), once per frame, not as it arrives.
        const Vec2 target = fixture.windowPixelOf(point + Vec3{1.0f, 0.0f, 0.0f});
        fixture.events({moved(target.x - 20.0f, target.y), moved(target.x, target.y)});
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
        static_cast<void>(fixture.viewports.update(1.0f / 60.0f));
        CHECK(same(fixture.translationOf(1), Vec3{1.0f, 0.0f, 0.0f}));
        CHECK(fixture.undoEntries() == entries);

        // A further move and the release, which counts its own position: one entry.
        const Vec2 further = fixture.windowPixelOf(point + Vec3{1.5f, 0.0f, 0.0f});
        fixture.events({moved(further.x, further.y), release(further.x, further.y)});
        CHECK(same(fixture.translationOf(1), Vec3{1.5f, 0.0f, 0.0f}));
        CHECK_FALSE(fixture.viewports.holdsDrag());
        CHECK(fixture.state().gizmoDrag == GizmoHandle::None);
        CHECK_FALSE(fixture.state().click.has_value()); // never a click
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        REQUIRE(fixture.undoEntries() == entries + 1);
        CHECK(fixture.session.document().history().entries().back().name == "Translate");

        run(fixture.session, "ith.edit.undo");
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
        run(fixture.session, "ith.edit.redo");
        CHECK(same(fixture.translationOf(1), Vec3{1.5f, 0.0f, 0.0f}));
    }

    TEST_CASE("a press on a handle and its release without a move record nothing") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const std::size_t entries = fixture.undoEntries();
        const Vec2 start = fixture.pixelOf(GizmoHandle::Y);
        fixture.frame({press(start.x, start.y)});
        fixture.frame({release(start.x, start.y)});
        CHECK(fixture.undoEntries() == entries);
        CHECK_FALSE(fixture.state().click.has_value());
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
    }

    TEST_CASE("Esc cancels a drag: the world is restored, nothing recorded, and nothing moves until the release") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const std::size_t entries = fixture.undoEntries();
        const Vec2 target = fixture.dragBy(GizmoHandle::Z, Vec3{0.0f, 0.0f, 1.0f});
        CHECK(same(fixture.translationOf(1), Vec3{0.0f, 0.0f, 1.0f}));

        fixture.frame({key(Key::Escape)});
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
        CHECK(fixture.state().gizmoDrag == GizmoHandle::None);
        CHECK(fixture.viewports.holdsDrag()); // until the release

        fixture.frame({moved(target.x + 30.0f, target.y + 10.0f)});
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
        fixture.frame({release(target.x + 30.0f, target.y + 10.0f)});
        CHECK_FALSE(fixture.viewports.holdsDrag());
        CHECK(fixture.undoEntries() == entries);
        CHECK_FALSE(fixture.state().click.has_value());
    }

    TEST_CASE("Alt at the press duplicates the selection and drags the copies, in one undo entry") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const std::size_t entries = fixture.undoEntries();
        const Vec2 target = fixture.dragBy(GizmoHandle::X, Vec3{2.0f, 0.0f, 0.0f}, KeyModifiers::Alt);
        CHECK(fixture.session.document().world().entityCount() == 2);
        const std::vector<ith::Uuid> copies = fixture.session.document().selection().entities();
        REQUIRE(copies.size() == 1);
        CHECK(copies[0] != idOf(1));

        SUBCASE("the release keeps the copy where it was dragged, the original where it was") {
            fixture.frame({release(target.x, target.y)});
            CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
            const ith::World& world = fixture.session.document().world();
            const ith::Entity copy = world.findByStableId(copies[0]);
            REQUIRE_FALSE(copy.isNull());
            CHECK(same(world.get<const ith::LocalTransform>(copy)->value.translation, Vec3{2.0f, 0.0f, 0.0f}));
            REQUIRE(fixture.undoEntries() == entries + 1);
            CHECK(fixture.session.document().history().entries().back().name == "Translate");

            // One undo removes the copy and selects the original again.
            run(fixture.session, "ith.edit.undo");
            CHECK(fixture.session.document().world().entityCount() == 1);
            CHECK(fixture.session.document().selection().entities() == std::vector<ith::Uuid>{idOf(1)});
        }
        SUBCASE("Esc removes the copy and selects the original again") {
            fixture.frame({key(Key::Escape)});
            CHECK(fixture.session.document().world().entityCount() == 1);
            CHECK(fixture.session.document().selection().entities() == std::vector<ith::Uuid>{idOf(1)});
            fixture.frame({release(target.x, target.y)});
            CHECK(fixture.undoEntries() == entries);
        }
    }

    TEST_CASE("a press off the handles is still a click, Alt + LMB off them still orbits, the centre clicks") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        // Far from the gizmo, near the image's corner.
        fixture.frame({press(110.0f, 110.0f)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Click);
        fixture.frame({release(110.0f, 110.0f)});
        CHECK(fixture.state().click.has_value());
        CHECK_FALSE(fixture.session.document().transactions().isOpen());

        fixture.frame({press(110.0f, 110.0f, KeyModifiers::Alt)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Orbit);
        fixture.frame({release(110.0f, 110.0f)});

        // The pivot, where every arrow starts, picks: a click on the selected shape still toggles it.
        const Vec2 centre = fixture.state().imagePosition + fixture.gizmo().layout.center;
        fixture.frame({press(centre.x, centre.y, KeyModifiers::Control)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Click);
        fixture.frame({release(centre.x, centre.y)});
        CHECK(fixture.state().click.has_value());
    }

    TEST_CASE("with another tool, in game view or during another interaction, a handle's place clicks") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const Vec2 arrow = fixture.pixelOf(GizmoHandle::X);

        SUBCASE("the rotate tool") {
            fixture.session.viewports().setTool(ViewportTool::Rotate);
        }
        SUBCASE("game view") {
            run(fixture.session, "ith.viewport.game_view");
        }
        SUBCASE("another interaction's transaction is open, as a Details field being typed into") {
            // The gizmo is hidden, so no handle is offered that a drag would refuse.
            ith::TransactionScope other = fixture.session.document().transactions().begin("Set Property");
            CHECK_FALSE(ith::viewportGizmo(fixture.session, fixture.state(), 1.0f).has_value());
            fixture.frame({press(arrow.x, arrow.y)});
            CHECK(fixture.viewports.gesture() == ViewportGesture::Click);
            fixture.frame({release(arrow.x, arrow.y)});
            CHECK(fixture.state().click.has_value());
            other.cancel();
            return;
        }
        fixture.frame({press(arrow.x, arrow.y)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Click);
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        fixture.frame({release(arrow.x, arrow.y)});
        CHECK(fixture.state().click.has_value());
    }

    TEST_CASE("snapping rounds the drag per axis to the step; without it the drag is exact") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        run(fixture.session, "ith.viewport.set_snap", R"({"step": 0.25})");
        Vec2 target = fixture.dragBy(GizmoHandle::X, Vec3{0.6f, 0.0f, 0.0f});
        CHECK(same(fixture.translationOf(1), Vec3{0.5f, 0.0f, 0.0f}));
        fixture.frame({release(target.x, target.y)});

        run(fixture.session, "ith.viewport.set_snap", R"({"enabled": false})");
        target = fixture.dragBy(GizmoHandle::X, Vec3{0.37f, 0.0f, 0.0f});
        CHECK(same(fixture.translationOf(1), Vec3{0.87f, 0.0f, 0.0f}));
        fixture.frame({release(target.x, target.y)});
    }

    TEST_CASE("a plane drag moves within the plane, snapped per axis") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f, 1.0f, 0.0f});
        const Vec2 target = fixture.dragBy(GizmoHandle::XZ, Vec3{0.52f, 0.0f, -0.31f});
        CHECK(same(fixture.translationOf(1), Vec3{0.5f, 1.0f, -0.3f}));
        fixture.frame({release(target.x, target.y)});
    }

    TEST_CASE(
        "a child under a rotated, scaled parent moves by the world delta; a selected child moves with its parent") {
        Fixture fixture;
        fixture.create(1, Vec3{0.5f, 0.0f, 0.0f}, std::nullopt, kQuarterTurn, Vec3{2.0f});
        fixture.create(2, Vec3{0.25f, 0.0f, 0.0f}, 1);
        const Vec3 before = fixture.worldPositionOf(2);
        CHECK(same(before, Vec3{0.5f, 0.0f, -0.5f}));

        SUBCASE("the child alone: its local translation is the world move in the parent's space") {
            const Vec2 target = fixture.dragBy(GizmoHandle::X, Vec3{1.0f, 0.0f, 0.0f});
            CHECK(same(fixture.worldPositionOf(2), before + Vec3{1.0f, 0.0f, 0.0f}));
            // A quarter turn and a scale of 2: one metre along world X is half a metre along local +Z.
            CHECK(same(fixture.translationOf(2), Vec3{0.25f, 0.0f, 0.5f}));
            fixture.frame({release(target.x, target.y)});
        }
        SUBCASE("the child and its parent: only the parent's translation changes") {
            fixture.select({2, 1});
            const Vec2 target = fixture.dragBy(GizmoHandle::Y, Vec3{0.0f, 1.0f, 0.0f});
            CHECK(same(fixture.translationOf(1), Vec3{0.5f, 1.0f, 0.0f}));
            CHECK(same(fixture.translationOf(2), Vec3{0.25f, 0.0f, 0.0f}));
            CHECK(same(fixture.worldPositionOf(2), before + Vec3{0.0f, 1.0f, 0.0f}));
            fixture.frame({release(target.x, target.y)});
        }
    }

    TEST_CASE("a drag held outside the image goes on, and its release there commits") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const std::size_t entries = fixture.undoEntries();
        const Vec2 start = fixture.pixelOf(GizmoHandle::X);
        const Vec3 point = fixture.grabbed(GizmoHandle::X, start);
        fixture.frame({press(start.x, start.y)});
        // Far along X, beyond the image's right edge.
        const Vec2 outside = fixture.windowPixelOf(point + Vec3{4.0f, 0.0f, 0.0f});
        REQUIRE(outside.x > 500.0f);
        fixture.frame({moved(outside.x, outside.y), release(outside.x, outside.y)});
        CHECK(same(fixture.translationOf(1), Vec3{4.0f, 0.0f, 0.0f}, 0.01f));
        CHECK(fixture.undoEntries() == entries + 1);
    }

    TEST_CASE("GizmoDrag refuses to begin during another interaction, with no selection or no grab") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const ith::ViewportGizmo gizmo = fixture.gizmo();
        const Vec2 pixel = fixture.pixelOf(GizmoHandle::X) - fixture.state().imagePosition;
        const ith::Ray ray = ith::pickRay(fixture.state().camera, fixture.state().imageSize, pixel);

        CHECK(ith::GizmoDrag::begin(fixture.session, GizmoHandle::None, gizmo.frame, ray, false).error().code ==
              "editor.gizmo.no_grab");
        {
            ith::TransactionScope other = fixture.session.document().transactions().begin("Other");
            CHECK(ith::GizmoDrag::begin(fixture.session, GizmoHandle::X, gizmo.frame, ray, false).error().code ==
                  "editor.command.interaction_open");
        }
        fixture.session.document().setPlaying(true);
        CHECK(ith::GizmoDrag::begin(fixture.session, GizmoHandle::X, gizmo.frame, ray, false).error().code ==
              "editor.command.playing");
        fixture.session.document().setPlaying(false);
        run(fixture.session, "ith.edit.select_none");
        CHECK(ith::GizmoDrag::begin(fixture.session, GizmoHandle::X, gizmo.frame, ray, false).error().code ==
              "editor.selection.empty");
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
    }

    TEST_CASE("GizmoDrag previews only a new delta, and a drag destroyed without commit cancels") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const ith::ViewportGizmo gizmo = fixture.gizmo();
        const ith::ViewportState& state = fixture.state();
        const Vec2 press = fixture.pixelOf(GizmoHandle::X) - state.imagePosition;
        const Vec3 point = fixture.grabbed(GizmoHandle::X, press + state.imagePosition);
        const std::size_t entries = fixture.undoEntries();
        {
            ith::Expected<ith::GizmoDrag> drag =
                ith::GizmoDrag::begin(fixture.session, GizmoHandle::X, gizmo.frame,
                                      ith::pickRay(state.camera, state.imageSize, press), false);
            REQUIRE(drag.hasValue());
            CHECK(drag->active());
            CHECK(drag->moved() == 1);
            const Vec2 target = fixture.windowPixelOf(point + Vec3{0.8f, 0.0f, 0.0f}) - state.imagePosition;
            const ith::Ray ray = ith::pickRay(state.camera, state.imageSize, target);
            REQUIRE(drag->preview(ray).hasValue());
            CHECK(same(drag->delta(), Vec3{0.8f, 0.0f, 0.0f}));
            const std::uint64_t values = fixture.session.document().changes().values();
            // The same delta again, and a snapped-equal one, change nothing.
            REQUIRE(drag->preview(ray).hasValue());
            const ith::Ray close =
                ith::pickRay(state.camera, state.imageSize, target + Vec2{0.5f, 0.0f}); // under half a step
            REQUIRE(drag->preview(close).hasValue());
            CHECK(fixture.session.document().changes().values() == values);
        }
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
        CHECK(fixture.undoEntries() == entries);
    }

    TEST_CASE("an Alt press on a handle released without a move cancels: no copy, no entry") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const std::size_t entries = fixture.undoEntries();
        const Vec2 start = fixture.pixelOf(GizmoHandle::X);
        fixture.frame({press(start.x, start.y, KeyModifiers::Alt)});
        CHECK(fixture.session.document().world().entityCount() == 2); // the copy, while held
        fixture.frame({release(start.x, start.y)});
        CHECK(fixture.session.document().world().entityCount() == 1);
        CHECK(fixture.undoEntries() == entries);
        CHECK(fixture.session.document().selection().entities() == std::vector<ith::Uuid>{idOf(1)});

        // Pressed again and again, the copies don't stack.
        for (int again = 0; again < 3; ++again) {
            fixture.frame({press(start.x, start.y, KeyModifiers::Alt)});
            fixture.frame({release(start.x, start.y)});
        }
        CHECK(fixture.session.document().world().entityCount() == 1);
        CHECK(fixture.undoEntries() == entries);
    }

    TEST_CASE("an Alt drag of a primary without a LocalTransform moves nothing and records nothing") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        run(fixture.session, "ith.entity.remove_component",
            R"({"ids": [")" + idText(1) + R"("], "type": "ith::LocalTransform"})");
        const std::size_t entries = fixture.undoEntries();
        const Vec2 target = fixture.dragBy(GizmoHandle::X, Vec3{1.0f, 0.0f, 0.0f}, KeyModifiers::Alt);
        fixture.frame({release(target.x, target.y)});
        CHECK(fixture.session.document().world().entityCount() == 1);
        CHECK(fixture.undoEntries() == entries);
    }

    TEST_CASE("redo of an Alt drag brings the copy back where it was dragged, selected") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const Vec2 target = fixture.dragBy(GizmoHandle::X, Vec3{2.0f, 0.0f, 0.0f}, KeyModifiers::Alt);
        fixture.frame({release(target.x, target.y)});
        const ith::Uuid copy = fixture.session.document().selection().entities().front();
        run(fixture.session, "ith.edit.undo");
        CHECK(fixture.session.document().world().entityCount() == 1);
        run(fixture.session, "ith.edit.redo");
        CHECK(fixture.session.document().world().entityCount() == 2);
        CHECK(fixture.session.document().selection().entities() == std::vector<ith::Uuid>{copy});
        const ith::World& world = fixture.session.document().world();
        CHECK(same(world.get<const ith::LocalTransform>(world.findByStableId(copy))->value.translation,
                   Vec3{2.0f, 0.0f, 0.0f}));
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
    }

    TEST_CASE("a drag moves every selected root by the same delta, in one entry") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        fixture.create(2, Vec3{-1.0f, 0.0f, 0.5f});
        fixture.create(3, Vec3{0.5f, 0.5f, 0.0f});
        fixture.select({1, 3, 2});
        const std::size_t entries = fixture.undoEntries();
        const Vec2 target = fixture.dragBy(GizmoHandle::Y, Vec3{0.0f, 1.0f, 0.0f});
        fixture.frame({release(target.x, target.y)});
        CHECK(same(fixture.translationOf(1), Vec3{0.0f, 1.0f, 0.0f}));
        CHECK(same(fixture.translationOf(2), Vec3{-1.0f, 1.0f, 0.5f}));
        CHECK(same(fixture.translationOf(3), Vec3{0.5f, 1.5f, 0.0f}));
        CHECK(fixture.undoEntries() == entries + 1);
    }

    TEST_CASE("losing focus cancels a held drag: nothing moves, nothing is recorded, Alt copies go") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const std::size_t entries = fixture.undoEntries();
        bool alt = false;
        SUBCASE("a plain drag") {}
        SUBCASE("an Alt drag") {
            alt = true;
        }
        static_cast<void>(
            fixture.dragBy(GizmoHandle::X, Vec3{1.0f, 0.0f, 0.0f}, alt ? KeyModifiers::Alt : KeyModifiers::None));
        CHECK(same(fixture.translationOf(1), alt ? Vec3{0.0f} : Vec3{1.0f, 0.0f, 0.0f}));
        // As the editor does: before routing a batch that holds the focus loss, whose synthetic
        // release must not commit.
        fixture.viewports.focusLost();
        CHECK(fixture.state().gizmoDrag == GizmoHandle::None);
        fixture.frame({Event{.type = EventType::WindowFocusLost}});
        CHECK_FALSE(fixture.viewports.holdsDrag());
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
        CHECK(fixture.session.document().world().entityCount() == 1);
        CHECK(fixture.undoEntries() == entries);
        CHECK_FALSE(fixture.state().click.has_value());
    }

    TEST_CASE("a drag that begins drops the clicks waiting to be picked") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        fixture.state().click = ith::ViewportClick{.position = {10.0f, 10.0f}};
        fixture.session.viewports().at(1).click = ith::ViewportClick{.position = {5.0f, 5.0f}};
        const Vec2 start = fixture.pixelOf(GizmoHandle::X);
        fixture.frame({press(start.x, start.y)});
        CHECK_FALSE(fixture.state().click.has_value());
        CHECK_FALSE(fixture.session.viewports().at(1).click.has_value());
        fixture.frame({release(start.x, start.y)});
    }

    TEST_CASE("after Esc, a new transaction or an undo before the release isn't touched by the dead drag") {
        Fixture fixture;
        fixture.create(1, Vec3{0.0f});
        const Vec2 target = fixture.dragBy(GizmoHandle::X, Vec3{1.0f, 0.0f, 0.0f});
        fixture.frame({key(Key::Escape)});
        const std::size_t entries = fixture.undoEntries();

        SUBCASE("a Details-like transaction stays open across the release") {
            ith::TransactionScope other = fixture.session.document().transactions().begin("Other");
            fixture.frame({release(target.x, target.y)});
            CHECK(other.active());
            CHECK(fixture.session.document().transactions().isOpen());
            other.cancel();
            CHECK(fixture.undoEntries() == entries);
        }
        SUBCASE("a duplicate made meanwhile stays") {
            run(fixture.session, "ith.edit.duplicate");
            fixture.frame({release(target.x, target.y)});
            CHECK(fixture.session.document().world().entityCount() == 2);
            CHECK(fixture.undoEntries() == entries + 1);
            CHECK(fixture.session.document().history().entries().back().name == "Duplicate");
        }
        SUBCASE("an undo meanwhile stays undone") {
            run(fixture.session, "ith.edit.undo"); // the create
            fixture.frame({release(target.x, target.y)});
            CHECK(fixture.session.document().world().entityCount() == 0);
            CHECK(fixture.undoEntries() == entries - 1);
        }
        CHECK_FALSE(fixture.viewports.holdsDrag());
    }

    TEST_CASE("a non-finite root skips the whole preview frame, so the roots never move apart") {
        Fixture fixture;
        // A ray from a camera at (0, 3, 5) through `point`.
        const auto rayTo = [](const Vec3& point) {
            const Vec3 camera{0.0f, 3.0f, 5.0f};
            return ith::Ray{.origin = camera, .direction = glm::normalize(point - camera)};
        };
        fixture.create(1, Vec3{0.0f});
        SUBCASE("a root whose translation is NaN") {
            fixture.create(2, Vec3{0.0f, 0.0f, -1.0f});
            fixture.session.document().mutableWorld().get<ith::LocalTransform>(fixture.entity(2))->value.translation.y =
                std::numeric_limits<float>::quiet_NaN();
        }
        SUBCASE("a root whose move overflows: the largest float under a parent scaled 1e-35") {
            fixture.create(3, Vec3{0.0f}, std::nullopt, ith::Transform{}.rotation, Vec3{1e-35f});
            fixture.create(2, Vec3{std::numeric_limits<float>::max(), 0.0f, 0.0f}, 3);
        }
        fixture.select({1, 2});
        ith::Expected<ith::GizmoDrag> drag = ith::GizmoDrag::begin(fixture.session, GizmoHandle::X, ith::GizmoFrame{},
                                                                   rayTo(Vec3{0.5f, 0.0f, 0.0f}), false);
        REQUIRE(drag.hasValue());
        CHECK(drag->moved() == 2);
        REQUIRE(drag->preview(rayTo(Vec3{1.5f, 0.0f, 0.0f})).hasValue());
        CHECK(same(fixture.translationOf(1), Vec3{0.0f}));
        CHECK(drag->delta() == Vec3{0.0f});
        drag->commit(); // nothing moved: cancelled
        CHECK_FALSE(fixture.session.document().transactions().isOpen());
    }

    TEST_CASE("a root under a tiny but uniform scale moves; one under a zero scale is skipped") {
        Fixture fixture;
        const auto rayTo = [](const Vec3& point) {
            const Vec3 camera{0.0f, 3.0f, 5.0f};
            return ith::Ray{.origin = camera, .direction = glm::normalize(point - camera)};
        };
        fixture.create(1, Vec3{0.0f}, std::nullopt, ith::Transform{}.rotation, Vec3{1e-20f});
        fixture.create(2, Vec3{0.0f}, 1);
        fixture.create(3, Vec3{0.0f}, std::nullopt, ith::Transform{}.rotation, Vec3{1.0f, 0.0f, 1.0f});
        fixture.create(4, Vec3{0.0f}, 3);
        fixture.select({4, 2});
        ith::Expected<ith::GizmoDrag> drag = ith::GizmoDrag::begin(fixture.session, GizmoHandle::X, ith::GizmoFrame{},
                                                                   rayTo(Vec3{0.5f, 0.0f, 0.0f}), false);
        REQUIRE(drag.hasValue());
        CHECK(drag->moved() == 1); // the zero-scale parent's child is skipped
        REQUIRE(drag->preview(rayTo(Vec3{1.5f, 0.0f, 0.0f})).hasValue());
        CHECK(same(fixture.worldPositionOf(2), Vec3{1.0f, 0.0f, 0.0f}));
        CHECK(fixture.translationOf(2).x == doctest::Approx(1e20f).epsilon(0.001));
        CHECK(same(fixture.translationOf(4), Vec3{0.0f}));
        drag->commit();
    }
}
