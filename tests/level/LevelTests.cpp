// Level documents (ADR-0034): the layout and byte-identical output of saveLevel, the entity mapping
// with its warnings, the hierarchy and root order rebuilt by loadLevel, references to later records,
// every validation error leaving the world unchanged, unresolved components written back, hooks run
// with ChildOf first, and levels loaded without the editor's types.

#include "ith/level/Level.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Hooks.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/LevelFormat.h"
#include "ith/level/UnresolvedComponents.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/Value.h"

#include "AssertCapture.h"
#include "EditorLevelComponents.h"
#include "LevelComponents.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(test_level)
ITH_DECLARE_REFLECTION_MODULE(test_level_editor)

namespace {

using ith::ChildOf;
using ith::Children;
using ith::Disabled;
using ith::DisabledInHierarchy;
using ith::Entity;
using ith::LocalTransform;
using ith::StableId;
using ith::UnresolvedComponents;
using ith::Uuid;
using ith::World;
using ith::serialization::Error;
using ith::serialization::Value;
using ith::test::level::Bulky;
using ith::test::level::EditorColor;
using ith::test::level::EditorNote;
using ith::test::level::Leg;
using ith::test::level::LevelSettings;
using ith::test::level::Light;
using ith::test::level::Marker;
using ith::test::level::Probe;
using ith::test::level::Renamed;
using ith::test::level::Sealed;
using ith::test::level::Shape;
using ith::test::level::Target;

constexpr Uuid kLevelId(0x6c0d5b0e8a514c33, 0x9e7f3f6f0a1b2c3d);

// What the Probe hooks saw, in order.
std::vector<std::string> g_probeLog;

void logProbe(ith::HookContext& context, std::string_view what) {
    const Entity entity = context.entity();
    g_probeLog.push_back(std::string(what) + ":" + std::to_string(context.value<const Probe>()->value) +
                         (context.has<ChildOf>(entity) ? ":parent" : ":root") +
                         (context.has<LocalTransform>(entity) ? ":local" : ""));
}

// The registry of a game with the editor: the ECS, the test types and the editor's test types.
const ith::TypeRegistry& editorRegistry() {
    static const bool kHooks = ith::setComponentHooks<Probe>({
        .onAdd = [](ith::HookContext& context) { logProbe(context, "add"); },
        .onRemove = [](ith::HookContext& context) { logProbe(context, "remove"); },
    });
    static const ith::TypeRegistry kRegistry = [] {
        ith::TypeRegistry registry;
        ith::generated::registerReflection_ecs(registry);
        ith::generated::registerReflection_test_level(registry);
        ith::generated::registerReflection_test_level_editor(registry);
        return registry;
    }();
    CHECK(kHooks);
    return kRegistry;
}

// The registry of a game without the editor: no EditorOnly types.
const ith::TypeRegistry& gameRegistry() {
    static_cast<void>(editorRegistry()); // the Probe hooks
    static const ith::TypeRegistry kRegistry = [] {
        ith::TypeRegistry registry;
        ith::generated::registerReflection_ecs(registry);
        ith::generated::registerReflection_test_level(registry);
        return registry;
    }();
    return kRegistry;
}

constexpr Uuid id(std::uint64_t n) {
    return Uuid(0, n);
}

// `text` with each "#<hex digit>" replaced by the text of id(<digit>).
std::string expandIds(std::string_view text) {
    std::string result;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '#' && at + 1 < text.size()) {
            const char digit = text[at + 1];
            const int value = digit >= 'a' ? digit - 'a' + 10 : digit - '0';
            result += id(static_cast<std::uint64_t>(value)).toString();
            ++at;
        } else {
            result += text[at];
        }
    }
    return result;
}

// A level document whose "entities" array is `entities`, with ids written as "#<hex digit>".
std::string document(std::string_view entities) {
    return expandIds(R"({"format": "ith.level", "version": 1, "id": ")" + kLevelId.toString() +
                     R"(", "data": {"entities": [)" + std::string(entities) + "]}}");
}

std::string save(const World& world, std::vector<Error>* warnings = nullptr) {
    const auto text = ith::saveLevel(world, kLevelId, editorRegistry(), {.warnings = warnings});
    REQUIRE(text.hasValue());
    return text.hasValue() ? *text : std::string();
}

ith::Expected<ith::LevelInfo, Error> load(std::string_view text, World& world, std::vector<Error>* warnings = nullptr,
                                          const ith::TypeRegistry& registry = editorRegistry()) {
    return ith::loadLevel(text, world, registry, {.source = "test.ithmap", .warnings = warnings});
}

std::vector<std::string> codes(const std::vector<Error>& errors) {
    std::vector<std::string> result;
    for (const Error& error : errors) {
        result.push_back(error.code);
    }
    return result;
}

Entity find(const World& world, std::uint64_t n) {
    return world.findByStableId(id(n));
}

// The stable ids (their low bits) of a list of entities; 0 for one without.
std::vector<std::uint64_t> ids(const World& world, const std::vector<Entity>& entities) {
    std::vector<std::uint64_t> result;
    for (const Entity entity : entities) {
        const StableId* stable = world.get<const StableId>(entity);
        result.push_back(stable != nullptr ? stable->id.low() : 0);
    }
    return result;
}

std::vector<std::uint64_t> rootIds(const World& world) {
    std::vector<Entity> roots;
    world.forEachRoot([&roots](Entity root) { roots.push_back(root); });
    return ids(world, roots);
}

std::vector<std::uint64_t> childIds(const World& world, std::uint64_t parent) {
    const Children* children = world.get<const Children>(find(world, parent));
    return children != nullptr ? ids(world, children->entities) : std::vector<std::uint64_t>{};
}

std::uint64_t parentId(const World& world, std::uint64_t child) {
    const ChildOf* link = world.get<const ChildOf>(find(world, child));
    return link != nullptr ? ids(world, {link->parent})[0] : 0;
}

// The document with every record's "editor" section removed, as the cooker strips it.
std::string stripEditor(std::string_view text) {
    auto document = ith::serialization::parseJson(text);
    REQUIRE(document.hasValue());
    for (Value& record : document->find("data")->find("entities")->elements()) {
        record.remove("editor");
    }
    const auto written = ith::serialization::writeJson(*document);
    REQUIRE(written.hasValue());
    return *written;
}

} // namespace

TEST_CASE("Level: kLevelFormat names the format") {
    CHECK(ith::kLevelFormat == "ith.level");
    CHECK(ith::kLevelVersion == 1);
}

TEST_CASE("Level: a level is saved in the documented layout") {
    static_cast<void>(editorRegistry());
    World world;
    const Entity lamp = world.spawn(StableId{.id = id(1)},
                                    Light{.intensity = 2.5f, .label = "lamp", .shape = Shape::Spot, .range = 0.1f},
                                    EditorNote{.text = "by the door"}, Marker{});
    world.spawn(StableId{.id = id(2)}, ChildOf{.parent = lamp, .despawnWithParent = true}, Target{.entity = lamp});

    const std::string expected = expandIds(R"({
  "format": "ith.level",
  "version": 1,
  "id": "6c0d5b0e-8a51-4c33-9e7f-3f6f0a1b2c3d",
  "data": {
    "entities": [
      {
        "id": "#1",
        "components": {
          "ith::test::level::Light": {
            "intensity": 2.5,
            "color": {
              "x": 1,
              "y": 1,
              "z": 1
            },
            "label": "lamp",
            "shape": "Spot",
            "range": 0.1,
            "flicker": []
          },
          "ith::test::level::Marker": {}
        },
        "editor": {
          "ith::test::level::EditorNote": {
            "text": "by the door"
          }
        }
      },
      {
        "id": "#2",
        "components": {
          "ith::ChildOf": {
            "parent": "#1",
            "despawnWithParent": true
          },
          "ith::test::level::Target": {
            "entity": "#1",
            "others": [],
            "route": []
          }
        }
      }
    ]
  }
}
)");
    std::vector<Error> warnings;
    CHECK(save(world, &warnings) == expected);
    CHECK(warnings.empty());
}

TEST_CASE("Level: the same world gives the same bytes, whatever order its components were added in") {
    static_cast<void>(editorRegistry());
    World first;
    first.spawn(StableId{.id = id(1)}, Light{.intensity = 0.3f}, Marker{}, EditorColor{.hue = 0.25f},
                EditorNote{.text = "a"});
    World second;
    const Entity entity = second.spawn(EditorNote{.text = "a"});
    second.add(entity, Marker{});
    second.add(entity, EditorColor{.hue = 0.25f});
    second.add(entity, Light{.intensity = 0.3f});
    second.add(entity, StableId{.id = id(1)});

    const std::string text = save(first);
    CHECK(save(first) == text);
    CHECK(save(second) == text);
    // Keys sorted bytewise, floats in their shortest form.
    CHECK(text.find(R"("intensity": 0.3,)") != std::string::npos);
    CHECK(text.find("EditorColor") < text.find("EditorNote"));
    CHECK(text.find("Light") < text.find("Marker"));
}

TEST_CASE("Level: 10,000 entities are saved, loaded and saved again to the same bytes") {
    static_cast<void>(editorRegistry());
    constexpr std::uint64_t kCount = 10'000;
    World world;
    std::vector<Entity> entities;
    for (std::uint64_t n = 0; n < kCount; ++n) {
        const Entity entity = world.spawn(StableId{.id = Uuid(0xABCD, n + 1)});
        entities.push_back(entity);
        // Every fifth entity is a root; the others hang below an earlier entity, so the tree is
        // several levels deep and the Outliner order differs from the spawn order.
        if (n % 5 != 0) {
            world.add(entity, ChildOf{.parent = entities[(n * 7) % n], .despawnWithParent = n % 3 == 0});
        }
        world.add(entity, LocalTransform{.value = {.translation = {static_cast<float>(n) * 0.1f, 1.0f / 3.0f,
                                                                   -static_cast<float>(n)}}});
        if (n % 2 == 0) {
            world.add(entity, Light{.intensity = static_cast<float>(n) / 7.0f,
                                    .label = "light " + std::to_string(n),
                                    .shape = static_cast<Shape>(n % 3),
                                    .flicker = {0.5f, static_cast<float>(n)}});
        }
        if (n % 11 == 0) {
            world.add(entity, EditorNote{.text = "note " + std::to_string(n)});
        }
        if (n % 97 == 0) {
            world.add(entity, Disabled{});
        }
    }
    // References forward and back, and to entities that will be saved later in Outliner order.
    for (std::uint64_t n = 0; n < kCount; n += 3) {
        world.add(entities[n], Target{.entity = entities[(n * 31 + 17) % kCount],
                                      .others = {entities[(n + 1) % kCount], Entity()},
                                      .route = {Leg{.to = entities[kCount - 1 - n], .wait = 0.5f}}});
    }
    world.placeBefore(entities[kCount - 5], entities[0]); // a reordered root

    std::vector<Error> warnings;
    const std::string text = save(world, &warnings);
    CHECK(warnings.empty());

    World loaded;
    const auto info = load(text, loaded, &warnings);
    REQUIRE(info.hasValue());
    CHECK(warnings.empty());
    CHECK(info->id == kLevelId);
    CHECK(info->entityCount == kCount);
    CHECK(loaded.entityCount() == kCount);
    CHECK(save(loaded) == text);

    // Loading into the world it came from replaces its entities and gives the same bytes.
    const auto again = load(text, world);
    REQUIRE(again.hasValue());
    CHECK(world.entityCount() == kCount);
    CHECK_FALSE(world.isAlive(entities[0]));
    CHECK(save(world) == text);
}

TEST_CASE("Level: loading rebuilds the hierarchy, the order of roots and children, and the enabled state") {
    static_cast<void>(editorRegistry());
    World world;
    const Entity a = world.spawn(StableId{.id = id(1)}, LocalTransform{.value = {.translation = {5, 0, 0}}});
    const Entity b = world.spawn(StableId{.id = id(2)});
    const Entity c = world.spawn(StableId{.id = id(3)}, Disabled{});
    const Entity a1 = world.spawn(StableId{.id = id(4)}, ChildOf{.parent = a},
                                  LocalTransform{.value = {.translation = {1, 2, 3}, .scale = {2, 2, 2}}});
    const Entity a2 = world.spawn(StableId{.id = id(5)}, ChildOf{.parent = a});
    const Entity a3 = world.spawn(StableId{.id = id(6)}, ChildOf{.parent = a});
    world.spawn(StableId{.id = id(7)}, ChildOf{.parent = a2});
    world.spawn(StableId{.id = id(8)}, ChildOf{.parent = c});
    world.placeBefore(c, a);   // roots: c, a, b
    world.placeBefore(a3, a1); // children of a: a3, a1, a2
    static_cast<void>(b);

    const std::string text = save(world);
    // Records in Outliner order: c, its child, a, its children depth-first, b.
    std::vector<std::size_t> positions;
    for (const std::uint64_t n : {3, 8, 1, 6, 4, 5, 7, 2}) {
        positions.push_back(text.find(R"("id": ")" + id(n).toString()));
    }
    CHECK(std::is_sorted(positions.begin(), positions.end()));

    World loaded;
    REQUIRE(load(text, loaded).hasValue());
    CHECK(rootIds(loaded) == std::vector<std::uint64_t>{3, 1, 2});
    CHECK(childIds(loaded, 1) == std::vector<std::uint64_t>{6, 4, 5});
    CHECK(childIds(loaded, 5) == std::vector<std::uint64_t>{7});
    CHECK(childIds(loaded, 3) == std::vector<std::uint64_t>{8});
    CHECK(parentId(loaded, 7) == 5);
    CHECK(loaded.get<const LocalTransform>(find(loaded, 4))->value == world.get<const LocalTransform>(a1)->value);
    CHECK(loaded.has<Disabled>(find(loaded, 3)));
    CHECK(loaded.has<DisabledInHierarchy>(find(loaded, 3)));
    CHECK(loaded.has<DisabledInHierarchy>(find(loaded, 8)));
    CHECK_FALSE(loaded.has<DisabledInHierarchy>(find(loaded, 1)));
    CHECK(save(loaded) == text);
}

TEST_CASE("Level: references may name records later in the file, ChildOf included") {
    World world;
    // Children before their parent, and references to entities further down.
    const std::string text = document(R"(
        {"id": "#1", "components": {"ith::ChildOf": {"parent": "#3"},
                                    "ith::test::level::Target": {"entity": "#4", "others": ["#3", "#1"]}}},
        {"id": "#2", "components": {"ith::ChildOf": {"parent": "#3"}}},
        {"id": "#3", "components": {"ith::ChildOf": {"parent": "#4"}}},
        {"id": "#4", "components": {}},
        {"id": "#5"})");
    std::vector<Error> warnings;
    const auto info = load(text, world, &warnings);
    REQUIRE(info.hasValue());
    CHECK(warnings.empty());
    CHECK(info->entityCount == 5);
    CHECK(rootIds(world) == std::vector<std::uint64_t>{4, 5});
    CHECK(childIds(world, 3) == std::vector<std::uint64_t>{1, 2});
    CHECK(childIds(world, 4) == std::vector<std::uint64_t>{3});
    const Target* target = world.get<const Target>(find(world, 1));
    REQUIRE(target != nullptr);
    CHECK(target->entity == find(world, 4));
    CHECK(target->others == std::vector<Entity>{find(world, 3), find(world, 1)});

    // Saved again in Outliner order.
    World again;
    REQUIRE(load(save(world), again).hasValue());
    CHECK(rootIds(again) == std::vector<std::uint64_t>{4, 5});
    CHECK(childIds(again, 3) == std::vector<std::uint64_t>{1, 2});
}

TEST_CASE("Level: every validation error leaves the world and the warnings unchanged") {
    static_cast<void>(editorRegistry());

    struct Case {
        const char* name;
        std::string text;
        const char* code;
        ith::serialization::UnknownFields unknownFields = ith::serialization::UnknownFields::Warn;
    };

    const auto fail = ith::serialization::UnknownFields::Fail;
    const std::string light = R"("ith::test::level::Light")";
    const std::vector<Case> cases = {
        {"not JSON", "{\"format\": ", "serialization.syntax"},
        {"another format", expandIds(R"({"format": "ith.prefab", "version": 1, "id": "#1", "data": {"entities": []}})"),
         "serialization.wrong_format"},
        {"no level id", R"({"format": "ith.level", "version": 1, "data": {"entities": []}})",
         "serialization.not_a_document"},
        {"a newer version", expandIds(R"({"format": "ith.level", "version": 2, "id": "#1", "data": {}})"),
         "serialization.newer_version"},
        {"data that isn't an object", expandIds(R"({"format": "ith.level", "version": 1, "id": "#1", "data": []})"),
         "serialization.wrong_type"},
        {"entities that aren't an array",
         expandIds(R"({"format": "ith.level", "version": 1, "id": "#1", "data": {"entities": {}}})"),
         "serialization.wrong_type"},
        {"entities given twice",
         expandIds(R"({"format": "ith.level", "version": 1, "id": "#1", "data": {"entities": [], "entities": []}})"),
         "serialization.duplicate_field"},
        {"a record that isn't an object", document("[]"), "serialization.wrong_type"},
        {"a record without an id", document(R"({"components": {}})"), "level.invalid_id"},
        {"an id that isn't a UUID", document(R"({"id": "door"})"), "level.invalid_id"},
        {"an id that isn't a string", document(R"({"id": 7})"), "level.invalid_id"},
        {"the nil id", document(R"({"id": "00000000-0000-0000-0000-000000000000"})"), "level.invalid_id"},
        {"an id given twice", document(R"({"id": "#1"}, {"id": "#2"}, {"id": "#1"})"), "level.duplicate_id"},
        {"a record key given twice", document(R"({"id": "#1", "id": "#2"})"), "serialization.duplicate_field"},
        {"components that aren't an object", document(R"({"id": "#1", "components": []})"), "serialization.wrong_type"},
        {"an editor section that isn't an object", document(R"({"id": "#1", "editor": 1})"),
         "serialization.wrong_type"},
        {"a StableId component", document(R"({"id": "#1", "components": {"ith::StableId": {"id": "#1"}}})"),
         "level.invalid_id"},
        {"a component given twice", document(R"({"id": "#1", "components": {)" + light + ": {}, " + light + ": {}}}"),
         "serialization.duplicate_field"},
        {"a component in both sections",
         document(R"({"id": "#1", "components": {)" + light + R"(: {}}, "editor": {)" + light + ": {}}}"),
         "serialization.duplicate_field"},
        {"a component under its name and its FormerName",
         document(R"({"id": "#1", "components": {"ith::test::level::Renamed": {},
                                                 "ith::test::level::OldName": {}}})"),
         "serialization.duplicate_field"},
        {"an entity that is its own parent",
         document(R"({"id": "#1", "components": {"ith::ChildOf": {"parent": "#1"}}})"), "level.parent_cycle"},
        {"a cycle of parents", document(R"(
            {"id": "#1", "components": {"ith::ChildOf": {"parent": "#3"}}},
            {"id": "#2", "components": {"ith::ChildOf": {"parent": "#1"}}},
            {"id": "#3", "components": {"ith::ChildOf": {"parent": "#2"}}},
            {"id": "#4", "components": {"ith::ChildOf": {"parent": "#3"}}})"),
         "level.parent_cycle"},
        {"an unknown component type when cooking", document(R"({"id": "#1", "components": {"game::Gone": {}}})"),
         "level.unresolved_component", fail},
        {"a Transient component type when cooking",
         document(R"({"id": "#1", "components": {"ith::test::level::Scratch": {}}})"), "level.unresolved_component",
         fail},
        {"an unreadable value when cooking", document(R"({"id": "#1", "components": {)" + light + R"(: 5}})"),
         "serialization.wrong_type", fail},
        {"an unknown field when cooking",
         document(R"({"id": "#1", "components": {)" + light + R"(: {"brightness": 1}}})"),
         "serialization.unknown_field", fail},
        {"an unknown record key when cooking", document(R"({"id": "#1", "prefab": "#2"})"),
         "serialization.unknown_field", fail},
        {"an unknown data key when cooking",
         expandIds(R"({"format": "ith.level", "version": 1, "id": "#1", "data": {"entities": [], "resources": {}}})"),
         "serialization.unknown_field", fail},
    };

    for (const Case& entry : cases) {
        CAPTURE(entry.name);
        World world;
        const Entity kept = world.spawn(StableId{.id = id(9)}, Light{.label = "kept"});
        world.spawn(StableId{.id = id(10)}, ChildOf{.parent = kept}, Marker{});
        const std::string before = save(world);

        std::vector<Error> warnings{Error{.code = "earlier"}};
        const auto result =
            ith::loadLevel(entry.text, world, editorRegistry(),
                           {.source = "bad.ithmap", .unknownFields = entry.unknownFields, .warnings = &warnings});
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == entry.code);
        CHECK(result.error().source == "bad.ithmap");
        CHECK(warnings.size() == 1);
        CHECK(world.isAlive(kept));
        CHECK(world.entityCount() == 2);
        CHECK(save(world) == before);
    }
}

TEST_CASE("Level: errors name where in the document they are") {
    static_cast<void>(editorRegistry());
    World world;
    auto result = load(document(R"({"id": "#1"}, {"id": "#2"}, {"id": "#1"})"), world);
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().path == "$.entities[2].id");

    result = ith::loadLevel(document(R"({"id": "#1", "components": {"ith::test::level::Light": {"label": 3}}})"), world,
                            editorRegistry(), {.unknownFields = ith::serialization::UnknownFields::Fail});
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().path == "$.entities[0].components.ith::test::level::Light.label");

    result = load(document(R"({"id": "#1"}, {"id": "#2", "components": {"ith::ChildOf": {"parent": "#2"}}})"), world);
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().path == "$.entities[1].components.ith::ChildOf");
}

TEST_CASE("Level: unknown and unreadable components are kept and written back") {
    // The input is written as saveLevel writes, so saving it again must give the same bytes.
    Value components = Value::object();
    const auto parsed = [](std::string_view json) {
        auto value = ith::serialization::parseJson(json);
        REQUIRE(value.hasValue());
        return value.hasValue() ? *value : Value();
    };
    components.set("game::Missing", parsed(R"({"a": [1, 2.50, {"b": null}], "c": "text"})"));
    components.set("ith::test::level::Light", parsed(R"({"intensity": "bright"})"));
    components.set("ith::test::level::Marker", Value::object());
    components.set("ith::test::level::NotAComponent", parsed(R"({"value": 1})"));
    components.set("ith::test::level::Scratch", parsed(R"({"value": 3})"));
    Value editor = Value::object();
    editor.set("game::EditorThing", parsed("[true]"));
    Value record = Value::object();
    record.set("id", Value::string(id(1).toString()));
    record.set("components", std::move(components));
    record.set("editor", std::move(editor));
    Value data = Value::object();
    data.set("entities", Value::array({std::move(record)}));
    constexpr ith::serialization::DocumentFormat kFormat{.name = ith::kLevelFormat, .version = 1};
    const auto written = ith::serialization::writeDocument(kFormat, data, &kLevelId);
    REQUIRE(written.hasValue());
    const std::string& text = *written;

    World world;
    std::vector<Error> warnings;
    REQUIRE(load(text, world, &warnings).hasValue());
    CHECK(codes(warnings) == std::vector<std::string>(5, "level.unresolved_component"));
    const Entity entity = find(world, 1);
    CHECK(world.has<Marker>(entity));
    CHECK_FALSE(world.has<Light>(entity));
    const UnresolvedComponents* unresolved = world.get<const UnresolvedComponents>(entity);
    REQUIRE(unresolved != nullptr);
    REQUIRE(unresolved->entries.size() == 5);
    CHECK(unresolved->entries[0].typeName == "game::Missing");
    CHECK_FALSE(unresolved->entries[0].editor);
    CHECK(unresolved->entries[4].typeName == "game::EditorThing");
    CHECK(unresolved->entries[4].editor);
    CHECK(warnings[1].path == "$.entities[0].components.ith::test::level::Light");

    std::vector<Error> saveWarnings;
    CHECK(save(world, &saveWarnings) == text);
    CHECK(saveWarnings.empty());

    // A real component replaces the unresolved entry of its name.
    world.add(entity, Light{.intensity = 4.0f});
    const std::string replaced = save(world);
    CHECK(replaced.find("bright") == std::string::npos);
    CHECK(replaced.find(R"("intensity": 4,)") != std::string::npos);
    CHECK(replaced.find("game::Missing") != std::string::npos);
}

TEST_CASE("Level: entities without a StableId, and references to them, are left out with warnings") {
    static_cast<void>(editorRegistry());
    World other;
    const Entity foreign = other.spawn(Marker{});

    World world;
    const Entity a = world.spawn(StableId{.id = id(1)});
    const Entity unsaved = world.spawn(Marker{});
    const Entity c = world.spawn(StableId{.id = id(3)}, ChildOf{.parent = unsaved});
    world.spawn(StableId{.id = id(4)}, ChildOf{.parent = c});
    const Entity helper = world.spawn(ChildOf{.parent = a}); // unsaved, without saved children
    const Entity stale = world.spawn(Marker{});
    world.despawn(stale);
    world.add(a, Target{.entity = unsaved, .others = {c, stale, foreign, Entity()}});
    static_cast<void>(helper);

    std::vector<Error> warnings;
    const std::string text = save(world, &warnings);
    CHECK(codes(warnings) == std::vector<std::string>{"level.unsaved_reference", "level.unsaved_reference",
                                                      "level.unsaved_reference", "level.dropped_parent",
                                                      "level.unsaved_entity"});
    CHECK(warnings[0].path == "$.entities[0].components.ith::test::level::Target");
    CHECK(warnings[0].message.find(id(1).toString()) != std::string::npos);
    CHECK(warnings[3].path == "$.entities[1]");
    CHECK(warnings[4].message == "2 entities have no StableId and are not saved");

    World loaded;
    std::vector<Error> loadWarnings;
    REQUIRE(load(text, loaded, &loadWarnings).hasValue());
    CHECK(loadWarnings.empty());
    CHECK(loaded.entityCount() == 3);
    CHECK(rootIds(loaded) == std::vector<std::uint64_t>{1, 3}); // c saved in its parent's place
    CHECK(parentId(loaded, 4) == 3);
    const Target* target = loaded.get<const Target>(find(loaded, 1));
    REQUIRE(target != nullptr);
    CHECK(target->entity.isNull());
    CHECK(target->others == std::vector<Entity>{find(loaded, 3), Entity(), Entity(), Entity()});
}

TEST_CASE("Level: an id that names no record loads as the null entity, and an unknown parent as a root") {
    World world;
    std::vector<Error> warnings;
    REQUIRE(load(document(R"(
        {"id": "#1", "components": {"ith::test::level::Target": {"entity": "#9", "route": [{"to": "#1"}]}}},
        {"id": "#2", "components": {"ith::ChildOf": {"parent": "#9"}}},
        {"id": "#3", "components": {"ith::ChildOf": {"parent": "00000000-0000-0000-0000-000000000000"}}})"),
                 world, &warnings)
                .hasValue());
    CHECK(codes(warnings) ==
          std::vector<std::string>{"level.unknown_reference", "level.dropped_parent", "level.dropped_parent"});
    CHECK(warnings[0].path == "$.entities[0].components.ith::test::level::Target");
    CHECK(warnings[0].source == "test.ithmap");
    const Target* target = world.get<const Target>(find(world, 1));
    REQUIRE(target != nullptr);
    CHECK(target->entity.isNull());
    CHECK(target->route[0].to == find(world, 1));
    CHECK(rootIds(world) == std::vector<std::uint64_t>{1, 2, 3});
    CHECK_FALSE(world.has<ChildOf>(find(world, 2)));
}

TEST_CASE("Level: loading adds components through World::add, ChildOf first, so hooks run") {
    World world;
    REQUIRE(load(document(R"(
        {"id": "#1", "components": {"ith::LocalTransform": {"value": {"translation": {"x": 10}}},
                                    "ith::test::level::Probe": {"value": 1}}},
        {"id": "#2", "components": {"ith::test::level::Probe": {"value": 2},
                                    "ith::LocalTransform": {"value": {"translation": {"x": 1}}},
                                    "ith::ChildOf": {"parent": "#1"},
                                    "ith::test::level::Sealed": {"value": 7}}})"),
                 world)
                .hasValue());
    // Probe comes before ChildOf in the record, yet its hook already sees the parent; components
    // after the parent follow in document order, so LocalTransform comes after Probe.
    CHECK(g_probeLog == std::vector<std::string>{"add:1:root:local", "add:2:parent"});
    // A LocalTransform added after the parent keeps its saved value instead of keeping the world
    // placement it had as a root.
    const Entity child = find(world, 2);
    CHECK(world.get<const LocalTransform>(child)->value.translation == ith::Vec3(1, 0, 0));
    CHECK(world.get<const Sealed>(child)->value == 7);
    CHECK(childIds(world, 1) == std::vector<std::uint64_t>{2});

    // Loading again clears the world first, running the removal hooks.
    g_probeLog.clear();
    REQUIRE(load(document(R"({"id": "#3"})"), world).hasValue());
    CHECK(g_probeLog.size() == 2);
    CHECK(g_probeLog[0].starts_with("remove:"));
    CHECK(world.entityCount() == 1);
    g_probeLog.clear();
}

TEST_CASE("Level: a level with its editor sections stripped loads without the editor's types") {
    World world;
    static_cast<void>(editorRegistry());
    const Entity root = world.spawn(StableId{.id = id(1)}, Light{}, EditorNote{.text = "note"}, EditorColor{});
    world.spawn(StableId{.id = id(2)}, ChildOf{.parent = root}, EditorNote{.text = "child"});
    const std::string text = save(world);

    const std::string stripped = stripEditor(text);
    CHECK(stripped.find("Editor") == std::string::npos);
    World game;
    std::vector<Error> warnings;
    REQUIRE(load(stripped, game, &warnings, gameRegistry()).hasValue());
    CHECK(warnings.empty());
    CHECK(game.entityCount() == 2);
    CHECK(game.has<Light>(find(game, 1)));
    CHECK_FALSE(game.has<UnresolvedComponents>(find(game, 1)));
    CHECK(parentId(game, 2) == 1);

    // Unstripped, the editor's components are kept unresolved and written back as they were;
    // a registry that knows their types reads them again.
    World unresolved;
    REQUIRE(load(text, unresolved, &warnings, gameRegistry()).hasValue());
    CHECK(codes(warnings) == std::vector<std::string>(3, "level.unresolved_component"));
    CHECK(save(unresolved) == text);
    World editor;
    REQUIRE(load(save(unresolved), editor).hasValue());
    CHECK(editor.get<const EditorNote>(find(editor, 2))->text == "child");
}

TEST_CASE("Level: loading keeps the world's resources and generations, so old handles stay stale") {
    static_cast<void>(editorRegistry());
    World world;
    const Entity old = world.spawn(StableId{.id = id(1)}, Marker{});
    world.insertResource(LevelSettings{.gravity = -1.0f});
    const std::string text = save(world);
    CHECK(text.find("gravity") == std::string::npos); // resources aren't saved

    REQUIRE(load(text, world).hasValue());
    CHECK_FALSE(world.isAlive(old));
    const Entity loaded = find(world, 1);
    CHECK(world.isAlive(loaded));
    CHECK(loaded != old);
    CHECK(world.has<Marker>(loaded));
    REQUIRE(world.resource<const LevelSettings>() != nullptr);
    CHECK(world.resource<const LevelSettings>()->gravity == -1.0f);

    // An empty level empties the world.
    const auto info = load(document(""), world);
    REQUIRE(info.hasValue());
    CHECK(info->entityCount == 0);
    CHECK(world.entityCount() == 0);
    CHECK(save(world).find(R"("entities": [])") != std::string::npos);
}

TEST_CASE("Level: saving fails on a nil level id and on values that can't be written") {
    static_cast<void>(editorRegistry());
    World world;
    world.spawn(StableId{.id = id(1)}, Light{.intensity = std::numeric_limits<float>::infinity()});
    auto result = ith::saveLevel(world, Uuid(), editorRegistry());
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().code == "serialization.invalid_id");

    std::vector<Error> warnings;
    result = ith::saveLevel(world, kLevelId, editorRegistry(), {.warnings = &warnings});
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().code == "serialization.non_finite");
    CHECK(result.error().path == "$.entities[0].components.ith::test::level::Light.intensity");
    CHECK(warnings.empty());
}

TEST_CASE("Level: saving and loading while the world is iterated assert and change nothing") {
    static_cast<void>(editorRegistry());
    World world;
    const Entity root = world.spawn(StableId{.id = id(1)});
    const std::string text = save(world);
    ith::test::AssertCapture capture;
    world.forEachRoot([&](Entity) {
        const auto loaded = load(text, world);
        REQUIRE_FALSE(loaded.hasValue());
        CHECK(loaded.error().code == "level.world_busy");
        const auto saved = ith::saveLevel(world, kLevelId, editorRegistry());
        REQUIRE_FALSE(saved.hasValue());
        CHECK(saved.error().code == "level.world_busy");
    });
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 2);
#endif
    CHECK(world.isAlive(root));
}

TEST_CASE("Level: a very deep and a very wide hierarchy load in linear time") {
    static_cast<void>(editorRegistry());
    // 100,000 entities in one chain, then 50,000 children of its root. Checking each link against
    // the entity's descendants, or searching a parent's children before each append, would take
    // minutes; the bound is generous for Debug and sanitizer builds.
    constexpr std::uint64_t kDepth = 100'000;
    constexpr std::uint64_t kWidth = 50'000;
    const auto stable = [](std::uint64_t n) { return Uuid(0xC0DE, n + 1).toString(); };
    std::string entities;
    entities.reserve((kDepth + kWidth) * 160);
    for (std::uint64_t n = 0; n < kDepth + kWidth; ++n) {
        entities += n > 0 ? ",\n" : "";
        entities += R"({"id": ")" + stable(n) + R"(")";
        if (n > 0) {
            const std::uint64_t parent = n < kDepth ? n - 1 : 0;
            entities += R"(, "components": {"ith::ChildOf": {"parent": ")" + stable(parent) + R"("}})";
        }
        entities += "}";
    }
    const std::string text = document(entities);

    World world;
    const auto start = std::chrono::steady_clock::now();
    const auto info = ith::loadLevel(text, world, editorRegistry());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(info.hasValue());
    CHECK(info->entityCount == kDepth + kWidth);
    CHECK(seconds < 120.0);

    const Entity root = world.findByStableId(Uuid(0xC0DE, 1));
    const Entity leaf = world.findByStableId(Uuid(0xC0DE, kDepth));
    REQUIRE(world.get<const ChildOf>(leaf) != nullptr);
    CHECK(world.get<const ChildOf>(leaf)->parent == world.findByStableId(Uuid(0xC0DE, kDepth - 1)));
    const Children* children = world.get<const Children>(root);
    REQUIRE(children != nullptr);
    CHECK(children->entities.size() == kWidth + 1);
    CHECK(children->entities[1] == world.findByStableId(Uuid(0xC0DE, kDepth + 1)));
    CHECK(children->entities.back() == world.findByStableId(Uuid(0xC0DE, kDepth + kWidth)));
}

TEST_CASE("Level: every value read from a level draws from one allocation budget") {
    static_cast<void>(editorRegistry());
    // 1,000 records of "{}" that each read into a 16 KiB value: a small tree, 16 MiB of values.
    std::string entities;
    for (std::uint64_t n = 1; n <= 1000; ++n) {
        entities += n > 1 ? ", " : "";
        entities += R"({"id": ")" + Uuid(0xB0, n).toString() + R"(", "components": {"ith::test::level::Bulky": {}}})";
    }
    const std::string text = document(entities);

    World world;
    const Entity kept = world.spawn(StableId{.id = id(1)});
    const auto refused = ith::loadLevel(text, world, editorRegistry(), {.allocationBudget = std::size_t{4} << 20});
    REQUIRE_FALSE(refused.hasValue());
    CHECK(refused.error().code == "serialization.too_large");
    CHECK(world.isAlive(kept));

    const auto loaded = ith::loadLevel(text, world, editorRegistry());
    REQUIRE(loaded.hasValue());
    CHECK(world.entityCount() == 1000);
    CHECK(world.has<Bulky>(world.findByStableId(Uuid(0xB0, 1000))));
}

TEST_CASE("Level: warnings are capped per code, and not built without a list") {
    static_cast<void>(editorRegistry());
    // 300 records with 50 unknown keys each, and an unknown field in a component value of each.
    std::string entities;
    for (std::uint64_t n = 1; n <= 300; ++n) {
        entities += n > 1 ? ", " : "";
        entities +=
            R"({"id": ")" + Uuid(0xA0, n).toString() + R"(", "components": {"ith::test::level::Light": {"glow": 1}})";
        for (int key = 0; key < 50; ++key) {
            entities += R"(, "k)" + std::to_string(key) + R"(": 0)";
        }
        entities += "}";
    }
    const std::string text = document(entities);

    World world;
    std::vector<Error> warnings;
    REQUIRE(load(text, world, &warnings).hasValue());
    REQUIRE(warnings.size() == ith::kMaxLevelWarningsPerCode + 1);
    CHECK(warnings.back().code == "serialization.unknown_field");
    CHECK(warnings.back().message == "15200 more serialization.unknown_field warnings are dropped");
    CHECK(warnings.back().source == "test.ithmap");

    World quiet;
    REQUIRE(ith::loadLevel(text, quiet, editorRegistry()).hasValue());
    CHECK(quiet.entityCount() == 300);
}

TEST_CASE("Level: an unresolved component the registry now knows doesn't make the save unloadable") {
    World world;
    std::vector<Error> warnings;
    // The value doesn't read, so the component is kept under its former name.
    REQUIRE(
        load(document(R"({"id": "#1", "components": {"ith::test::level::OldName": {"value": "x"}}})"), world, &warnings)
            .hasValue());
    CHECK(codes(warnings) == std::vector<std::string>{"level.unresolved_component"});
    const Entity entity = find(world, 1);
    world.add(entity, Renamed{.value = 5});

    // Writing both names would be refused as one type given twice; the real component wins.
    const std::string text = save(world);
    CHECK(text.find("OldName") == std::string::npos);
    World again;
    REQUIRE(load(text, again).hasValue());
    CHECK(again.get<const Renamed>(find(again, 1))->value == 5);
    CHECK_FALSE(again.has<UnresolvedComponents>(find(again, 1)));
}

TEST_CASE("Level: names from the file are printable and short in messages and paths") {
    World world;
    const std::string key = "game::" + std::string(300, 'x') + '\x01';
    const std::string json = "game::" + std::string(300, 'x') + R"(\u0001)"; // the key as JSON escapes it
    std::vector<Error> warnings;
    REQUIRE(
        load(document(R"({"id": "#1", "components": {")" + json + R"(": {}}, ")" + json + R"(": 1})"), world, &warnings)
            .hasValue());
    REQUIRE(warnings.size() == 2);
    for (const Error& warning : warnings) {
        CAPTURE(warning.path);
        CHECK(warning.path.size() < 120);
        CHECK(warning.path.ends_with("..."));
        CHECK(warning.message.size() < 300);
        CHECK(warning.message.find('\x01') == std::string::npos);
    }
    // The kept value still has its full name, so it is saved back as it was.
    CHECK(world.get<const UnresolvedComponents>(find(world, 1))->entries[0].typeName == key);
}
