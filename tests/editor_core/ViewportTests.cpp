// The viewports (ADR-0042 rules 3 to 5, 20 and 26 to 29): the Viewports store and its defaults, the
// per-level cameras in Saved/Editor/Viewports.json (round trip, written only when changed, missing
// and unreadable files), the session saving them when a level closes, the tool, gizmo-space, snap
// and game-view commands, Focus (fitting the selection's world bounds, disabled with an empty
// selection, not undoable) and Snap to Floor (the ground, stacked shapes, hanging below every
// support, children following their roots under rotated, scaled and collapsed parents, entities
// without bounds, disabled shapes, one undo entry), and the WorldTransforms an opened level gets.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/TransformPropagation.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Viewports.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/math/Geometry.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"
#include "ith/shapes/ShapeMesh.h"
#include "ith/shapes/ShapeRenderer.h"

#include "AssertCapture.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ith::EditorCamera;
using ith::EditorSession;
using ith::Vec3;
using ith::Viewports;
using ith::test::editor::fails;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::json;
using ith::test::editor::run;
using ith::test::editor::TempProject;
using ith::test::editor::types;

constexpr float kEpsilon = 1e-4f;

bool same(const Vec3& a, const Vec3& b) {
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

bool same(const ith::Vec4& a, const ith::Vec4& b) {
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

std::string number(float value) {
    // The shortest text that reads back as the same float, so rotations stay exactly unit length.
    std::array<char, 32> text{};
    const std::to_chars_result written = std::to_chars(text.data(), text.data() + text.size(), value);
    return {text.data(), written.ptr};
}

std::string vec(const Vec3& v) {
    return R"({"x": )" + number(v.x) + R"(, "y": )" + number(v.y) + R"(, "z": )" + number(v.z) + "}";
}

// Creates entity n with a transform, optionally a shape, a parent and a rotation; it becomes the
// selection.
void create(EditorSession& session, std::uint64_t n, const Vec3& translation, std::optional<ith::Shape> shape,
            const Vec3& scale = Vec3(1.0f), std::optional<std::uint64_t> parent = std::nullopt,
            const ith::Quat& rotation = ith::Transform{}.rotation) {
    std::string components = R"({"ith::LocalTransform": {"value": {"translation": )" + vec(translation) +
                             R"(, "rotation": {"x": )" + number(rotation.x) + R"(, "y": )" + number(rotation.y) +
                             R"(, "z": )" + number(rotation.z) + R"(, "w": )" + number(rotation.w) + R"(}, "scale": )" +
                             vec(scale) + "}}";
    if (shape.has_value()) {
        constexpr std::array<std::string_view, 4> kNames{"Cube", "Sphere", "Plane", "Cylinder"};
        components += R"(, "ith::ShapeRenderer": {"shape": ")" +
                      std::string(kNames.at(static_cast<std::size_t>(*shape))) + R"("})";
    }
    components += "}";
    std::string args = R"({"id": ")" + idText(n) + R"(", "components": )" + components;
    if (parent.has_value()) {
        args += R"(, "parent": ")" + idText(*parent) + R"(")";
    }
    args += "}";
    run(session, "ith.entity.create", args);
}

Vec3 translationOf(const EditorSession& session, std::uint64_t n) {
    const ith::World& world = session.document().world();
    const ith::Entity entity = world.findByStableId(idOf(n));
    REQUIRE_FALSE(entity.isNull());
    return world.get<const ith::LocalTransform>(entity)->value.translation;
}

void select(EditorSession& session, const std::vector<std::uint64_t>& ids) {
    std::string list;
    for (const std::uint64_t n : ids) {
        list += (list.empty() ? "\"" : ", \"") + idText(n) + "\"";
    }
    run(session, "ith.selection.set", R"({"entities": [)" + list + "]}");
}

// A camera other than the default.
EditorCamera movedCamera(float x) {
    return EditorCamera{.position = {x, 2.0f, 5.0f},
                        .yaw = 0.5f,
                        .pitch = -0.25f,
                        .fovX = 1.2f,
                        .speedStep = 6,
                        .pivot = {x, 0.0f, 0.0f}};
}

// Counts the warnings of the EditorViewports category while it lives.
class ViewportWarnings {
public:
    ViewportWarnings() { REQUIRE(ith::addLogSink(m_buffer.sink()).hasValue()); }

    ViewportWarnings(const ViewportWarnings&) = delete;
    ViewportWarnings& operator=(const ViewportWarnings&) = delete;
    ViewportWarnings(ViewportWarnings&&) = delete;
    ViewportWarnings& operator=(ViewportWarnings&&) = delete;

    ~ViewportWarnings() { ith::removeLogSink(m_buffer.sink()); }

    [[nodiscard]] std::size_t count() {
        std::vector<ith::LogEntry> entries;
        m_next = m_buffer.read(m_next, entries);
        for (const ith::LogEntry& entry : entries) {
            if (entry.level == ith::LogLevel::Warning &&
                std::string_view(entry.category->name()) == "EditorViewports") {
                ++m_count;
            }
        }
        return m_count;
    }

private:
    ith::LogRingBuffer m_buffer{256};
    std::uint64_t m_next = 0;
    std::size_t m_count = 0;
};

bool exists(const std::string& path) {
    const auto info = ith::fs::fileInfo(path);
    return info.hasValue() && info->has_value();
}

} // namespace

TEST_SUITE("editor_core.viewports") {
    TEST_CASE("the store holds four viewports with default cameras and the editor-wide tool state") {
        Viewports viewports;
        for (std::size_t i = 0; i < ith::kMaxViewports; ++i) {
            const std::string id = Viewports::instanceId(i);
            CAPTURE(id);
            CHECK(id == "ith.viewport." + std::to_string(i + 1));
            const ith::ViewportState* state = viewports.find(id);
            REQUIRE(state != nullptr);
            CHECK(state->camera == EditorCamera{});
            CHECK_FALSE(state->gameView);
            CHECK(state->aspect() == 1.0f);
        }
        for (const std::string_view other : {"ith.viewport", "ith.viewport.0", "ith.viewport.5", "ith.viewport.12",
                                             "ith.outliner", "ith.viewport.1 "}) {
            CAPTURE(other);
            CHECK(viewports.find(other) == nullptr);
        }
        CHECK(viewports.tool() == ith::ViewportTool::Translate);
        CHECK(viewports.gizmoSpace() == ith::GizmoSpace::World);
        CHECK(viewports.snapEnabled());
        CHECK(viewports.snapStep() == doctest::Approx(0.1f));
        CHECK(ith::kViewportNearPlane == doctest::Approx(0.1f));

        // Rule 5: the default camera sits back, right and above, looking at the origin.
        const EditorCamera camera;
        CHECK(same(camera.position, Vec3(3.6742346f, 3.0f, 3.6742346f)));
        CHECK(camera.yaw == doctest::Approx(0.25f * ith::kPi));
        CHECK(camera.pitch == doctest::Approx(-ith::kPi / 6.0f));
        CHECK(camera.fovX == doctest::Approx(ith::kHalfPi));
        CHECK(camera.speedStep == 4);
        CHECK(same(camera.pivot, Vec3(0.0f)));

        ith::ViewportState* first = viewports.find("ith.viewport.1");
        first->imageSize = {800.0f, 400.0f};
        CHECK(first->aspect() == doctest::Approx(2.0f));
        first->imageSize = {800.0f, 0.0f};
        CHECK(first->aspect() == 1.0f);

        viewports.setSnap(false, 0.5f);
        CHECK_FALSE(viewports.snapEnabled());
        CHECK(viewports.snapStep() == doctest::Approx(0.5f));
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("an invalid snapping step and viewport index are asserted and change nothing") {
        ith::test::AssertCapture capture;
        Viewports viewports;
        viewports.setSnap(false, 0.0f);
        viewports.setSnap(false, -1.0f);
        CHECK(capture.count() == 2);
        CHECK(viewports.snapEnabled());
        CHECK(viewports.snapStep() == doctest::Approx(0.1f));
        CHECK(Viewports::instanceId(7) == "ith.viewport.4");
        CHECK(capture.count() == 3);
    }
#endif

    TEST_CASE("cameras round-trip per level through the viewports file, written only when they changed") {
        TempProject project;
        const std::string file = ith::fs::joinPath(project.root(), ith::kViewportsFile);
        const ith::Uuid levelA(1, 1);
        const ith::Uuid levelB(1, 2);
        {
            Viewports viewports(file);
            // A level whose cameras are all the default gets no entry, and nothing is written.
            viewports.saveLevel(ith::Uuid(1, 9));
            CHECK_FALSE(exists(file));
            viewports.find("ith.viewport.1")->camera = movedCamera(1.0f);
            viewports.find("ith.viewport.3")->camera = movedCamera(3.0f);
            viewports.saveLevel(levelA);
            REQUIRE(exists(file));
            viewports.find("ith.viewport.1")->camera = movedCamera(7.0f);
            viewports.saveLevel(levelB);
        }
        const auto text = ith::fs::readFile(file, ith::kMaxViewportsFileSize);
        REQUIRE(text.hasValue());
        CHECK(text->find(R"("format": "ith.editor_viewports")") != std::string::npos);
        CHECK(text->find(levelA.toString()) != std::string::npos);
        CHECK(text->find("ith.viewport.4") != std::string::npos);

        Viewports reread(file);
        reread.find("ith.viewport.2")->gameView = true;
        reread.restoreLevel(levelA);
        CHECK(reread.find("ith.viewport.1")->camera == movedCamera(1.0f));
        CHECK(reread.find("ith.viewport.2")->camera == EditorCamera{});
        CHECK(reread.find("ith.viewport.3")->camera == movedCamera(3.0f));
        CHECK(reread.find("ith.viewport.2")->gameView); // session state, not saved
        reread.restoreLevel(levelB);
        CHECK(reread.find("ith.viewport.1")->camera == movedCamera(7.0f));
        reread.restoreLevel(ith::Uuid(1, 3));
        CHECK(reread.find("ith.viewport.1")->camera == EditorCamera{});

        // Saving what the file already holds doesn't write it; a change does.
        reread.restoreLevel(levelA);
        REQUIRE(ith::fs::remove(file).hasValue());
        reread.saveLevel(levelA);
        CHECK_FALSE(exists(file));
        reread.find("ith.viewport.4")->camera.speedStep = 2;
        reread.saveLevel(levelA);
        CHECK(exists(file));

        // Without a file the cameras are kept for the session only.
        Viewports memory;
        memory.find("ith.viewport.1")->camera = movedCamera(2.0f);
        memory.saveLevel(levelA);
        memory.restoreLevel(levelB);
        CHECK(memory.find("ith.viewport.1")->camera == EditorCamera{});
        memory.restoreLevel(levelA);
        CHECK(memory.find("ith.viewport.1")->camera == movedCamera(2.0f));
    }

    TEST_CASE("a missing viewports file means default cameras; an unreadable one also warns once") {
        TempProject project;
        const std::string file = ith::fs::joinPath(project.root(), ith::kViewportsFile);
        const ith::Uuid level(1, 1);
        {
            ViewportWarnings warnings;
            Viewports viewports(file);
            viewports.restoreLevel(level);
            viewports.restoreLevel(level);
            CHECK(warnings.count() == 0); // a fresh project has none yet
            CHECK(viewports.find("ith.viewport.1")->camera == EditorCamera{});
            // Nothing kept, nothing to write.
            CHECK_FALSE(exists(file));
        }
        const std::string valid = R"({"format": "ith.editor_viewports", "version": 1, "data": {"levels": {")" +
                                  level.toString() + R"(": {"ith.viewport.1": {"speedStep": 7}}}}})";
        const std::string speedStep = R"("speedStep": 7)";
        const auto withStep = [&](std::string_view replacement) {
            return valid.substr(0, valid.find(speedStep)) + std::string(replacement) +
                   valid.substr(valid.find(speedStep) + speedStep.size());
        };
        // A missing field keeps its default; the horizontal field of view is read (ADR-0046).
        for (const auto& [text, fovX] :
             {std::pair{valid, ith::kHalfPi}, std::pair{withStep(R"("speedStep": 7, "fovX": 1.25)"), 1.25f}}) {
            CAPTURE(text);
            REQUIRE(ith::fs::writeFileAtomic(file, text).hasValue());
            ViewportWarnings warnings;
            Viewports viewports(file);
            viewports.restoreLevel(level);
            CHECK(warnings.count() == 0);
            CHECK(viewports.find("ith.viewport.1")->camera.speedStep == 7);
            CHECK(viewports.find("ith.viewport.1")->camera.fovX == doctest::Approx(fovX));
        }
        // A camera saved with the vertical fovY of before ADR-0046 loads silently: its position, yaw
        // and pitch are kept, fovY is ignored and fovX is the default.
        {
            REQUIRE(ith::fs::writeFileAtomic(
                        file, withStep(R"("speedStep": 7, "position": {"x": 1, "y": 2, "z": 3}, "yaw": 0.5, )"
                                       R"("pitch": -0.25, "fovY": 1.2)"))
                        .hasValue());
            ViewportWarnings warnings;
            Viewports viewports(file);
            viewports.restoreLevel(level);
            CHECK(warnings.count() == 0);
            const EditorCamera& camera = viewports.find("ith.viewport.1")->camera;
            CHECK(same(camera.position, Vec3(1.0f, 2.0f, 3.0f)));
            CHECK(camera.yaw == doctest::Approx(0.5f));
            CHECK(camera.pitch == doctest::Approx(-0.25f));
            CHECK(camera.speedStep == 7);
            CHECK(camera.fovX == doctest::Approx(ith::kHalfPi));
        }
        const std::string badStep = withStep(R"("speedStep": 12)");
        for (const std::string& bad :
             {std::string("not json"), std::string(R"({"format": "ith.editor_layout", "version": 1, "data": {}})"),
              std::string(R"({"format": "ith.editor_viewports", "version": 2, "data": {"levels": {}}})"),
              std::string(R"({"format": "ith.editor_viewports", "version": 1, "data": {"levels": []}})"),
              std::string(R"({"format": "ith.editor_viewports", "version": 1, "data": {"levels": {"nope": {}}}})"),
              badStep}) {
            CAPTURE(bad);
            REQUIRE(ith::fs::writeFileAtomic(file, bad).hasValue());
            ViewportWarnings warnings;
            Viewports viewports(file);
            viewports.restoreLevel(level);
            CHECK(warnings.count() == 1);
            CHECK(viewports.find("ith.viewport.1")->camera == EditorCamera{});
            // The next save replaces the unreadable file with a good one.
            viewports.find("ith.viewport.1")->camera = movedCamera(4.0f);
            viewports.saveLevel(level);
            Viewports reread(file);
            reread.restoreLevel(level);
            CHECK(warnings.count() == 1);
            CHECK(reread.find("ith.viewport.1")->camera == movedCamera(4.0f));
        }
    }

    TEST_CASE("the session keeps a level's cameras when it closes and restores them when it opens") {
        TempProject project;
        const std::string file = ith::fs::joinPath(project.root(), ith::kViewportsFile);
        EditorSession session(types(), {.projectDirectory = project.root()});
        // An untitled level keeps no cameras.
        session.viewports().find("ith.viewport.1")->camera = movedCamera(1.0f);
        session.saveViewports();
        CHECK_FALSE(exists(file));

        run(session, "ith.file.save_as", R"({"path": "Content/Room.ithmap"})");
        const ith::Uuid room = session.document().levelId();
        session.viewports().find("ith.viewport.2")->camera = movedCamera(2.0f);
        run(session, "ith.file.new_level");
        CHECK(exists(file));
        CHECK(session.viewports().find("ith.viewport.1")->camera == EditorCamera{});
        CHECK(session.viewports().find("ith.viewport.2")->camera == EditorCamera{});

        run(session, "ith.file.open_level", R"({"path": "Content/Room.ithmap"})");
        REQUIRE(session.document().levelId() == room);
        CHECK(session.viewports().find("ith.viewport.1")->camera == movedCamera(1.0f));
        CHECK(session.viewports().find("ith.viewport.2")->camera == movedCamera(2.0f));

        // At exit: a new session of the project finds them.
        session.viewports().find("ith.viewport.1")->camera = movedCamera(5.0f);
        session.saveViewports();
        EditorSession next(types(), {.projectDirectory = project.root()});
        run(next, "ith.file.open_level", R"({"path": "Content/Room.ithmap"})");
        CHECK(next.viewports().find("ith.viewport.1")->camera == movedCamera(5.0f));
    }

    TEST_CASE("after a Save As the cameras follow the level id: kept for the copy, or for the level overwritten") {
        TempProject project;
        EditorSession session(types(), {.projectDirectory = project.root()});
        run(session, "ith.file.save_as", R"({"path": "Content/Room.ithmap"})");
        const ith::Uuid room = session.document().levelId();
        run(session, "ith.file.new_level"); // Room closes with the default cameras: no entry

        run(session, "ith.file.open_level", R"({"path": "Content/Room.ithmap"})");
        session.viewports().find("ith.viewport.1")->camera = movedCamera(3.0f);
        run(session, "ith.file.save_as", R"({"path": "Content/Copy.ithmap"})");
        const ith::Uuid copy = session.document().levelId();
        REQUIRE(copy != room);
        // The cameras stay as they are through the save.
        CHECK(session.viewports().find("ith.viewport.1")->camera == movedCamera(3.0f));
        run(session, "ith.file.new_level");
        run(session, "ith.file.open_level", R"({"path": "Content/Copy.ithmap"})");
        CHECK(session.viewports().find("ith.viewport.1")->camera == movedCamera(3.0f));
        run(session, "ith.file.open_level", R"({"path": "Content/Room.ithmap"})");
        CHECK(session.viewports().find("ith.viewport.1")->camera == EditorCamera{});

        // Over Room: the level is Room now, and so are its cameras when it closes.
        run(session, "ith.file.open_level", R"({"path": "Content/Copy.ithmap"})");
        session.viewports().find("ith.viewport.1")->camera = movedCamera(6.0f);
        run(session, "ith.file.save_as", R"({"path": "Content/Room.ithmap"})");
        REQUIRE(session.document().levelId() == room);
        run(session, "ith.file.new_level");
        run(session, "ith.file.open_level", R"({"path": "Content/Room.ithmap"})");
        CHECK(session.viewports().find("ith.viewport.1")->camera == movedCamera(6.0f));
        run(session, "ith.file.open_level", R"({"path": "Content/Copy.ithmap"})");
        CHECK(session.viewports().find("ith.viewport.1")->camera == movedCamera(3.0f));
    }

    TEST_CASE("the tool, gizmo space, snap and game-view commands change the store, not the level") {
        EditorSession session(types());
        Viewports& viewports = session.viewports();
        CHECK(run(session, "ith.viewport.rotate_tool").find("tool")->asString() == "rotate");
        CHECK(viewports.tool() == ith::ViewportTool::Rotate);
        run(session, "ith.viewport.scale_tool");
        CHECK(viewports.tool() == ith::ViewportTool::Scale);
        run(session, "ith.viewport.cycle_tool");
        CHECK(viewports.tool() == ith::ViewportTool::Translate);
        run(session, "ith.viewport.cycle_tool");
        CHECK(viewports.tool() == ith::ViewportTool::Rotate);
        run(session, "ith.viewport.translate_tool");
        CHECK(viewports.tool() == ith::ViewportTool::Translate);

        run(session, "ith.viewport.toggle_gizmo_space");
        CHECK(viewports.gizmoSpace() == ith::GizmoSpace::Local);
        run(session, "ith.viewport.toggle_gizmo_space");
        CHECK(viewports.gizmoSpace() == ith::GizmoSpace::World);

        run(session, "ith.viewport.set_snap", R"({"enabled": false})");
        CHECK_FALSE(viewports.snapEnabled());
        CHECK(viewports.snapStep() == doctest::Approx(0.1f));
        run(session, "ith.viewport.set_snap", R"({"step": 0.5})");
        CHECK_FALSE(viewports.snapEnabled());
        CHECK(viewports.snapStep() == doctest::Approx(0.5f));
        for (const std::string_view bad : {R"({"step": 0})", R"({"step": -1})", R"({"step": "big"})",
                                           R"({"enabled": "yes"})", R"({"step": 1e300})"}) {
            CAPTURE(bad);
            fails(session, "ith.viewport.set_snap", bad, "editor.command.invalid_args");
        }
        CHECK(viewports.snapStep() == doctest::Approx(0.5f));

        run(session, "ith.viewport.game_view");
        CHECK(viewports.find("ith.viewport.1")->gameView);
        CHECK_FALSE(viewports.find("ith.viewport.3")->gameView);
        run(session, "ith.viewport.game_view", R"({"viewport": "ith.viewport.3"})");
        CHECK(viewports.find("ith.viewport.3")->gameView);
        fails(session, "ith.viewport.game_view", R"({"viewport": "ith.outliner"})", "editor.command.invalid_args");

        const auto immersive = session.execute("ith.viewport.immersive");
        REQUIRE_FALSE(immersive.hasValue());
        CHECK(immersive.error().code == "editor.command.disabled");
        CHECK(immersive.error().message.find("not in P2") != std::string::npos);

        CHECK_FALSE(session.document().dirty());
        CHECK_FALSE(session.document().history().canUndo());
    }

    TEST_CASE("Focus frames the selection's world bounds in the given viewport and is not undoable") {
        EditorSession session(types());
        const auto disabled = session.commands().canExecute(session, "ith.viewport.focus_selection");
        REQUIRE_FALSE(disabled.hasValue());
        CHECK(disabled.error().code == "editor.command.disabled");
        CHECK(disabled.error().message.find("Select something to focus on") != std::string::npos);
        fails(session, "ith.viewport.focus_selection", "{}", "editor.command.disabled");

        create(session, 1, {2.0f, 1.0f, -3.0f}, ith::Shape::Cube, {2.0f, 1.0f, 4.0f});
        create(session, 2, {-4.0f, 6.0f, 0.0f}, std::nullopt);
        create(session, 3, {0.0f, 0.0f, 9.0f}, ith::Shape::Sphere, Vec3(1.0f), 2); // below the empty entity
        const std::size_t entries = session.document().history().entries().size();
        const bool dirty = session.document().dirty();

        select(session, {1});
        ith::ViewportState* second = session.viewports().find("ith.viewport.2");
        second->imageSize = {1600.0f, 800.0f};
        run(session, "ith.viewport.focus_selection", R"({"viewport": "ith.viewport.2"})");
        const ith::Aabb cube{.min = {1.0f, 0.5f, -5.0f}, .max = {3.0f, 1.5f, -1.0f}};
        EditorCamera expected;
        ith::focus(expected, cube, 2.0f);
        CHECK(same(second->camera.pivot, expected.pivot));
        CHECK(same(second->camera.position, expected.position));
        CHECK(session.viewports().find("ith.viewport.1")->camera == EditorCamera{});

        // An entity without a shape adds its position; a child's bounds follow its parent.
        select(session, {1, 2, 3});
        run(session, "ith.viewport.focus_selection");
        const ith::Aabb sphere{.min = {-4.5f, 5.5f, 8.5f}, .max = {-3.5f, 6.5f, 9.5f}};
        const ith::Aabb all{.min = glm::min(cube.min, sphere.min), .max = glm::max(cube.max, sphere.max)};
        EditorCamera framed;
        ith::focus(framed, all, 1.0f);
        CHECK(same(session.viewports().find("ith.viewport.1")->camera.pivot, all.center()));
        CHECK(same(session.viewports().find("ith.viewport.1")->camera.position, framed.position));

        CHECK(session.document().history().entries().size() == entries);
        CHECK(session.document().dirty() == dirty);
    }

    TEST_CASE("Snap to Floor drops shapes onto the ground and onto the tops of shapes below them") {
        EditorSession session(types());
        create(session, 1, {0.0f, 0.5f, 0.0f}, ith::Shape::Cube);                          // a base on the ground
        create(session, 2, {0.3f, 4.0f, 0.0f}, ith::Shape::Cube);                          // above the base
        create(session, 3, {10.0f, 3.0f, 0.0f}, ith::Shape::Cube, {2.0f, 2.0f, 2.0f});     // alone
        create(session, 4, {0.0f, -3.0f, 0.2f}, ith::Shape::Cube);                         // under the base
        create(session, 5, {3.0f, 5.0f, 0.0f}, ith::Shape::Cylinder, {1.0f, 10.0f, 1.0f}); // tall, apart
        create(session, 6, {0.0f, 7.0f, 0.0f}, std::nullopt);                              // no bounds
        create(session, 7, {0.0f, 1.5f, 3.0f}, ith::Shape::Plane);                         // a flat support
        create(session, 8, {0.0f, 6.0f, 3.0f}, ith::Shape::Sphere);                        // above the plane

        select(session, {2, 3, 4, 6, 8});
        const std::string before = ith::test::editor::levelText(session);
        const std::size_t entries = session.document().history().entries().size();
        run(session, "ith.viewport.snap_to_floor");
        CHECK(same(translationOf(session, 1), {0.0f, 0.5f, 0.0f}));
        CHECK(same(translationOf(session, 2), {0.3f, 1.5f, 0.0f}));  // on the base
        CHECK(same(translationOf(session, 3), {10.0f, 1.0f, 0.0f})); // on the ground
        CHECK(same(translationOf(session, 4), {0.0f, 1.5f, 0.2f}));  // below every support: the highest
        CHECK(same(translationOf(session, 6), {0.0f, 7.0f, 0.0f}));  // skipped
        CHECK(same(translationOf(session, 8), {0.0f, 2.0f, 3.0f}));  // on the plane

        // One undo entry, named by the command.
        REQUIRE(session.document().history().entries().size() == entries + 1);
        CHECK(session.document().history().entries().back().name == "Snap to Floor");
        run(session, "ith.edit.undo");
        CHECK(ith::test::editor::levelText(session) == before);
        run(session, "ith.edit.redo");
        CHECK(same(translationOf(session, 2), {0.3f, 1.5f, 0.0f}));

        // What already rests makes no entry.
        const std::size_t after = session.document().history().entries().size();
        run(session, "ith.viewport.snap_to_floor");
        CHECK(session.document().history().entries().size() == after);
        fails(session, "ith.viewport.snap_to_floor", R"({"ids": []})", "editor.command.invalid_args");
    }

    TEST_CASE("Snap to Floor moves roots; children follow and never support their root") {
        EditorSession session(types());
        create(session, 1, {0.0f, 5.0f, 0.0f}, ith::Shape::Cube);
        create(session, 2, {0.0f, -2.0f, 0.0f}, ith::Shape::Cube, Vec3(1.0f), 1); // world y 3, under its root
        create(session, 3, {10.0f, 4.0f, 0.0f}, std::nullopt, Vec3(2.0f));        // a scaled parent
        create(session, 4, {0.0f, 0.0f, 0.0f}, ith::Shape::Cube, Vec3(1.0f), 3);  // world bottom 3

        select(session, {1, 2, 4});
        run(session, "ith.viewport.snap_to_floor");
        CHECK(same(translationOf(session, 1), {0.0f, 0.5f, 0.0f}));  // its child doesn't hold it up
        CHECK(same(translationOf(session, 2), {0.0f, -2.0f, 0.0f})); // a selected child follows its root
        // The world drop of 3 m is 1.5 m in the parent's doubled scale.
        CHECK(same(translationOf(session, 4), {0.0f, -1.5f, 0.0f}));
        CHECK(same(translationOf(session, 3), {10.0f, 4.0f, 0.0f}));
    }

    TEST_CASE("Snap to Floor under rotated, scaled and collapsed parents; a disabled shape is no support") {
        EditorSession session(types());
        const ith::Quat quarterTurn = glm::angleAxis(ith::kHalfPi, Vec3(0.0f, 0.0f, 1.0f));
        // A parent turned 90 degrees about Z: its local +X is world +Y.
        create(session, 1, {0.0f, 5.0f, 0.0f}, std::nullopt, Vec3(1.0f), std::nullopt, quarterTurn);
        create(session, 2, {2.0f, 0.0f, 0.0f}, ith::Shape::Cube, Vec3(1.0f), 1); // world (0, 7, 0)
        // Turned and stretched along its local X (world Y) by 2.
        create(session, 3, {10.0f, 5.0f, 0.0f}, std::nullopt, {2.0f, 1.0f, 1.0f}, std::nullopt, quarterTurn);
        create(session, 4, {1.0f, 0.0f, 0.0f}, ith::Shape::Cube, Vec3(1.0f), 3); // world (10, 7, 0), 2 m tall
        // A parent with a zero scale: no local translation can move its child.
        create(session, 5, {20.0f, 5.0f, 0.0f}, std::nullopt, {1.0f, 0.0f, 1.0f});
        create(session, 6, {0.0f, 0.0f, 0.0f}, ith::Shape::Cube, Vec3(1.0f), 5);

        select(session, {2, 4, 6});
        const std::size_t entries = session.document().history().entries().size();
        run(session, "ith.viewport.snap_to_floor");
        CHECK(same(translationOf(session, 2), {-4.5f, 0.0f, 0.0f}));
        CHECK(same(session.document().world().worldMatrixOf(session.document().world().findByStableId(idOf(2)))[3],
                   ith::Vec4(0.0f, 0.5f, 0.0f, 1.0f)));
        CHECK(same(translationOf(session, 4), {-2.0f, 0.0f, 0.0f}));
        CHECK(same(session.document().world().worldMatrixOf(session.document().world().findByStableId(idOf(4)))[3],
                   ith::Vec4(10.0f, 1.0f, 0.0f, 1.0f)));
        CHECK(same(translationOf(session, 6), {0.0f, 0.0f, 0.0f})); // skipped
        CHECK(session.document().history().entries().size() == entries + 1);

        // Rounding under the turned parents leaves no residue that a second End would record.
        run(session, "ith.viewport.snap_to_floor");
        CHECK(session.document().history().entries().size() == entries + 1);

        // Only the collapsed child: nothing moves and nothing is recorded.
        select(session, {6});
        run(session, "ith.viewport.snap_to_floor");
        CHECK(session.document().history().entries().size() == entries + 1);

        // A disabled shape holds nothing up.
        create(session, 7, {30.0f, 0.5f, 0.0f}, ith::Shape::Cube);
        create(session, 8, {30.0f, 4.0f, 0.0f}, ith::Shape::Cube);
        run(session, "ith.entity.set_enabled", R"({"ids": [")" + idText(7) + R"("], "enabled": false})");
        select(session, {8});
        run(session, "ith.viewport.snap_to_floor");
        CHECK(same(translationOf(session, 8), {30.0f, 0.5f, 0.0f}));
    }

    TEST_CASE("an opened level's placed entities get a WorldTransform, without an undo entry") {
        TempProject project;
        {
            EditorSession session(types(), {.projectDirectory = project.root()});
            create(session, 1, {1.0f, 2.0f, 3.0f}, ith::Shape::Cube);
            create(session, 2, {0.0f, 1.0f, 0.0f}, ith::Shape::Sphere, Vec3(1.0f), 1);
            run(session, "ith.file.save_as", R"({"path": "Content/Shapes.ithmap"})");
        }
        EditorSession session(types(), {.projectDirectory = project.root()});
        run(session, "ith.file.open_level", R"({"path": "Content/Shapes.ithmap"})");
        ith::World& world = session.document().mutableWorld();
        const ith::Entity parent = world.findByStableId(idOf(1));
        const ith::Entity child = world.findByStableId(idOf(2));
        REQUIRE(world.has<ith::WorldTransform>(parent));
        REQUIRE(world.has<ith::WorldTransform>(child));
        ith::propagateTransforms(world);
        CHECK(same(world.get<const ith::WorldTransform>(child)->matrix[3], ith::Vec4(1.0f, 3.0f, 3.0f, 1.0f)));
        CHECK_FALSE(session.document().dirty());
        CHECK_FALSE(session.document().history().canUndo());
    }
}
