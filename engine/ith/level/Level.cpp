#include "ith/level/Level.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Entity.h"
#include "ith/reflection/Entities.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Value.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {
namespace {

using serialization::Value;

constexpr serialization::DocumentFormat kLevelDocument{
    .name = kLevelFormat, .version = kLevelVersion, .minimumVersion = kLevelVersion};

constexpr std::string_view kEntitiesKey = "entities";
constexpr std::string_view kIdKey = "id";
constexpr std::string_view kComponentsKey = "components";
constexpr std::string_view kEditorKey = "editor";

serialization::Error makeError(std::string code, std::string message, std::string_view source, std::string path = {}) {
    return serialization::Error{
        .code = std::move(code), .message = std::move(message), .source = std::string(source), .path = std::move(path)};
}

std::string recordPath(std::size_t record) {
    return "$." + std::string(kEntitiesKey) + "[" + std::to_string(record) + "]";
}

std::string componentPath(std::size_t record, bool editor, std::string_view typeName) {
    return recordPath(record) + "." + std::string(editor ? kEditorKey : kComponentsKey) + "." + std::string(typeName);
}

// Moves an error or warning about a component value, whose path starts at "$" (the value), under
// the component's place in the document.
serialization::Error placed(serialization::Error error, const std::string& prefix, std::string_view source) {
    const std::string_view inner =
        error.path.starts_with('$') ? std::string_view(error.path).substr(1) : std::string_view(error.path);
    error.path = prefix + std::string(inner);
    if (error.source.empty()) {
        error.source = std::string(source);
    }
    return error;
}

bool isTransient(const TypeInfo& type) {
    return hasFlags(type.flags, ReflectionFlags::Transient);
}

bool isEditorOnly(const TypeInfo& type) {
    return hasFlags(type.flags, ReflectionFlags::EditorOnly);
}

serialization::Error worldBusy(std::string_view source) {
    return makeError("level.world_busy",
                     "the world is being iterated, or a system or hook runs on it; levels are saved and loaded "
                     "between frames",
                     source);
}

// ---------------------------------------------------------------------------------------------
// Saving

struct SaveContext {
    const World* world = nullptr;
    const Uuid* referrer = nullptr; // the stable id of the entity being written
    std::string_view typeName;      // the component being written
    std::string path;               // its place in the document
    std::vector<serialization::Error>* warnings = nullptr;
};

// The entity mapping of a save: an entity is written as its stable id, and an entity that isn't
// saved as the nil id, with a warning.
// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMapping::toId has
Uuid toStableId(void* context, Entity entity) {
    const SaveContext& save = *static_cast<const SaveContext*>(context);
    // A handle of another world would assert in the lookup, so it is checked first.
    if (entity.worldId() == save.world->id()) {
        if (const auto* id = save.world->get<const StableId>(entity)) {
            return id->id;
        }
    }
    save.warnings->push_back(makeError("level.unsaved_reference",
                                       "entity " + save.referrer->toString() + ": " + std::string(save.typeName) +
                                           " refers to an entity that is not saved (no StableId, stale, or of "
                                           "another world); the reference is saved as the nil id",
                                       {}, save.path));
    return Uuid{};
}

// One component of a record being written.
struct SavedComponent {
    std::string_view typeName;
    bool editor = false;
    Value value;
};

// Writes the saved components of `entity`, record `recordIndex`, into `components`, sorted by type
// name: its components except Transient ones, StableId and, when `parentSaved` is false, ChildOf;
// then the entries of its UnresolvedComponents whose type name it doesn't have as a component.
Expected<void, serialization::Error> collectComponents(const World& world, Entity entity, bool parentSaved,
                                                       std::size_t recordIndex, SaveContext& context,
                                                       std::vector<SavedComponent>& components) {
    const serialization::EntityMapping mapping{.toId = &toStableId, .context = &context};
    std::vector<std::pair<const TypeInfo*, const void*>> values;
    world.forEachComponent(entity,
                           [&values](const TypeInfo& type, const void* value) { values.emplace_back(&type, value); });
    components.clear();
    for (const auto& [type, value] : values) {
        if (isTransient(*type) || type->id == typeOf<StableId>().id) {
            continue;
        }
        if (type->id == typeOf<ChildOf>().id && !parentSaved) {
            context.warnings->push_back(makeError("level.dropped_parent",
                                                  "entity " + context.referrer->toString() +
                                                      ": its parent has no StableId and is not saved; it is saved "
                                                      "as a root",
                                                  {}, recordPath(recordIndex)));
            continue;
        }
        context.typeName = type->name;
        context.path = componentPath(recordIndex, isEditorOnly(*type), type->name);
        Expected<Value, serialization::Error> written = serialization::toValue(*type, value, &mapping);
        if (!written) {
            return makeUnexpected(placed(std::move(written).error(), context.path, {}));
        }
        components.push_back({.typeName = type->name, .editor = isEditorOnly(*type), .value = std::move(*written)});
    }
    if (const auto* unresolved = world.get<const UnresolvedComponents>(entity)) {
        const auto resolved = static_cast<std::ptrdiff_t>(components.size());
        for (const UnresolvedComponent& entry : unresolved->entries) {
            const bool replaced =
                std::ranges::any_of(components.begin(), components.begin() + resolved,
                                    [&entry](const SavedComponent& saved) { return saved.typeName == entry.typeName; });
            if (!replaced) {
                components.push_back({.typeName = entry.typeName, .editor = entry.editor, .value = entry.value});
            }
        }
    }
    // Bytewise: std::char_traits<char> compares as unsigned char.
    std::ranges::sort(components,
                      [](const SavedComponent& a, const SavedComponent& b) { return a.typeName < b.typeName; });
    return {};
}

// The record of an entity with stable id `id`, whose components are moved out of `components`.
Value makeRecord(const Uuid& id, std::vector<SavedComponent>& components) {
    Value saved = Value::object();
    Value editor = Value::object();
    for (SavedComponent& component : components) {
        Value& section = component.editor ? editor : saved;
        section.members().push_back({.key = std::string(component.typeName), .value = std::move(component.value)});
    }
    Value record = Value::object();
    record.members().push_back({.key = std::string(kIdKey), .value = Value::string(id.toString())});
    record.members().push_back({.key = std::string(kComponentsKey), .value = std::move(saved)});
    if (!editor.members().empty()) {
        record.members().push_back({.key = std::string(kEditorKey), .value = std::move(editor)});
    }
    return record;
}

} // namespace

Expected<std::string, serialization::Error> saveLevel(const World& world, const Uuid& levelId,
                                                      const LevelSaveOptions& options) {
    ITH_ASSERT_MSG(!world.isIterating(), "level: saveLevel while the world is iterated or a system or hook runs");
    if (world.isIterating()) {
        return makeUnexpected(worldBusy({}));
    }
    if (levelId.isNil()) {
        return makeUnexpected(makeError("serialization.invalid_id", "a level's id can't be the nil id", {}));
    }

    std::vector<serialization::Error> warnings;
    SaveContext context{.world = &world, .warnings = &warnings};

    // Outliner order: each root in root order, then depth-first its children in Children order.
    // An explicit stack, so a deep hierarchy can't overflow the call stack.
    struct Pending {
        Entity entity;
        bool parentSaved = true; // false: the parent has no StableId, so the ChildOf isn't written
    };

    std::vector<Pending> stack;
    world.forEachRoot([&stack](Entity root) { stack.push_back({.entity = root}); });
    std::ranges::reverse(stack);

    Value entities = Value::array();
    std::size_t unsavedCount = 0;
    std::vector<SavedComponent> components;
    while (!stack.empty()) {
        const Pending next = stack.back();
        stack.pop_back();
        const auto* stableId = world.get<const StableId>(next.entity);
        if (const auto* children = world.get<const Children>(next.entity)) {
            for (const Entity child : std::views::reverse(children->entities)) {
                stack.push_back({.entity = child, .parentSaved = stableId != nullptr});
            }
        }
        if (stableId == nullptr) {
            ++unsavedCount;
            continue;
        }
        context.referrer = &stableId->id;
        const Expected<void, serialization::Error> collected =
            collectComponents(world, next.entity, next.parentSaved, entities.elements().size(), context, components);
        if (!collected) {
            return makeUnexpected(collected.error());
        }
        entities.elements().push_back(makeRecord(stableId->id, components));
    }
    if (unsavedCount > 0) {
        warnings.push_back(makeError("level.unsaved_entity",
                                     std::to_string(unsavedCount) +
                                         (unsavedCount == 1 ? " entity has" : " entities have") + " no StableId and " +
                                         (unsavedCount == 1 ? "is" : "are") + " not saved",
                                     {}));
    }

    Value data = Value::object();
    data.members().push_back({.key = std::string(kEntitiesKey), .value = std::move(entities)});
    Expected<std::string, serialization::Error> text = serialization::writeDocument(kLevelDocument, data, &levelId);
    if (text && options.warnings != nullptr) {
        options.warnings->insert(options.warnings->end(), std::make_move_iterator(warnings.begin()),
                                 std::make_move_iterator(warnings.end()));
    }
    return text;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Loading

constexpr std::uint32_t kNoRecord = std::numeric_limits<std::uint32_t>::max();

// While a document is read, an Entity value holds a placeholder: the index of the record it names,
// under a world id no handle of the build phase is compared with. Building turns each placeholder
// into the entity spawned for that record.
constexpr std::uint8_t kPlaceholderWorld = 1;

Entity placeholder(std::uint32_t record) {
    return Entity::fromParts(record, 0, kPlaceholderWorld);
}

struct LoadContext {
    const std::unordered_map<Uuid, std::uint32_t>* records = nullptr;
    const Uuid* referrer = nullptr; // the record being read
    std::string_view typeName;      // the component being read
    bool childOf = false;           // ChildOf reports an unknown parent as a dropped parent instead
    std::string path;               // the component's place in the document
    std::string_view source;
    std::vector<serialization::Error> warnings; // of the component being read, kept only when it reads
};

// The entity mapping of a load: an id names the record that has it, as a placeholder.
Entity toPlaceholder(void* context, const Uuid& id) {
    LoadContext& load = *static_cast<LoadContext*>(context);
    if (const auto found = load.records->find(id); found != load.records->end()) {
        return placeholder(found->second);
    }
    if (!load.childOf) {
        load.warnings.push_back(makeError("level.unknown_reference",
                                          "entity " + load.referrer->toString() + ": " + std::string(load.typeName) +
                                              " refers to " + id.toString() +
                                              ", which no entity of the level has; it reads as the null entity",
                                          load.source, load.path));
    }
    return {};
}

// The entity spawned for a placeholder.
// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMap::function has
Entity fromPlaceholder(void* context, Entity entity) {
    const auto& spawned = *static_cast<const std::vector<Entity>*>(context);
    if (entity.isNull()) {
        return {};
    }
    ITH_ASSERT(entity.index() < spawned.size());
    return entity.index() < spawned.size() ? spawned[entity.index()] : Entity{};
}

// One record, read and checked.
struct LoadedRecord {
    Uuid id;
    const Value* components = nullptr;
    const Value* editor = nullptr;
    OwnedValue childOf;             // empty without a (kept) parent
    std::vector<OwnedValue> values; // the other components, in document order
    std::vector<UnresolvedComponent> unresolved;
};

class LevelReader {
public:
    LevelReader(const TypeRegistry& registry, const LevelLoadOptions& options, std::size_t budget)
        : m_registry(registry), m_options(options), m_budget(budget) {}

    // Reads and checks the document's data. On success the records are ready to build.
    Expected<void, serialization::Error> read(const Value& data) {
        if (data.kind() != Value::Kind::Object) {
            return fail("serialization.wrong_type", "a level's data is an object with 'entities'", "$");
        }
        const Value* entities = nullptr;
        for (const Value::Member& member : data.members()) {
            if (member.key == kEntitiesKey) {
                if (entities != nullptr) {
                    return fail("serialization.duplicate_field", "'entities' is given twice", "$.entities");
                }
                entities = &member.value;
            } else if (auto unknown = unknownKey("$." + member.key); !unknown) {
                return unknown;
            }
        }
        if (entities == nullptr) {
            return {}; // a missing field keeps its default: no entities
        }
        if (entities->kind() != Value::Kind::Array) {
            return fail("serialization.wrong_type", "'entities' is an array of entity records", "$.entities");
        }
        if (entities->elements().size() >= kNoRecord) {
            return fail("serialization.too_large", "a level has fewer than 2^32 - 1 entities", "$.entities");
        }

        m_records.resize(entities->elements().size());
        m_ids.reserve(m_records.size());
        for (std::size_t index = 0; index < m_records.size(); ++index) {
            if (auto shape = readShape(entities->elements()[index], index); !shape) {
                return shape;
            }
        }
        m_parents.assign(m_records.size(), kNoRecord);
        for (std::size_t index = 0; index < m_records.size(); ++index) {
            if (auto components = readComponents(index); !components) {
                return components;
            }
        }
        return checkCycles();
    }

    // Replaces the world's entities with the records. Can't fail.
    void build(World& world) {
        world.clear();
        std::vector<Entity> spawned;
        spawned.reserve(m_records.size());
        for (const LoadedRecord& record : m_records) {
            spawned.push_back(world.spawn(StableId{.id = record.id}));
            ITH_ASSERT(!spawned.back().isNull());
        }
        const EntityMap map{.function = &fromPlaceholder, .context = &spawned};
        for (LoadedRecord& record : m_records) {
            if (record.childOf.hasValue()) {
                remapEntities(*record.childOf.type(), record.childOf.get(), map);
            }
            for (OwnedValue& value : record.values) {
                remapEntities(*value.type(), value.get(), map);
            }
        }
        for (std::size_t index = 0; index < m_records.size(); ++index) {
            LoadedRecord& record = m_records[index];
            const Entity entity = spawned[index];
            // The parent first: a LocalTransform added after it keeps its saved value.
            if (record.childOf.hasValue()) {
                [[maybe_unused]] const void* added = world.add(entity, *record.childOf.type(), record.childOf.get());
                ITH_ASSERT(added != nullptr);
            }
            for (OwnedValue& value : record.values) {
                [[maybe_unused]] const void* added = world.add(entity, *value.type(), value.get());
                ITH_ASSERT(added != nullptr);
            }
            if (!record.unresolved.empty()) {
                world.add(entity, UnresolvedComponents{.entries = std::move(record.unresolved)});
            }
        }
    }

    [[nodiscard]] std::size_t recordCount() const noexcept { return m_records.size(); }

    [[nodiscard]] std::vector<serialization::Error>& warnings() noexcept { return m_warnings; }

private:
    Unexpected<serialization::Error> fail(std::string code, std::string message, std::string path) const {
        return makeUnexpected(makeError(std::move(code), std::move(message), m_options.source, std::move(path)));
    }

    [[nodiscard]] bool failsUnknown() const noexcept {
        return m_options.unknownFields == serialization::UnknownFields::Fail;
    }

    // An unknown key of the data or of a record: a warning, or an error with UnknownFields::Fail.
    Expected<void, serialization::Error> unknownKey(std::string path) {
        serialization::Error error =
            makeError("serialization.unknown_field", "unknown field; it is dropped", m_options.source, std::move(path));
        if (failsUnknown()) {
            error.message = "unknown field";
            return makeUnexpected(std::move(error));
        }
        m_warnings.push_back(std::move(error));
        return {};
    }

    // Checks a record's keys and id, and notes its sections.
    Expected<void, serialization::Error> readShape(const Value& value, std::size_t index) {
        const std::string path = recordPath(index);
        if (value.kind() != Value::Kind::Object) {
            return fail("serialization.wrong_type", "an entity record is an object with 'id' and 'components'", path);
        }
        LoadedRecord& record = m_records[index];
        const Value* id = nullptr;
        for (const Value::Member& member : value.members()) {
            const Value** slot = nullptr;
            if (member.key == kIdKey) {
                slot = &id;
            } else if (member.key == kComponentsKey) {
                slot = &record.components;
            } else if (member.key == kEditorKey) {
                slot = &record.editor;
            }
            if (slot == nullptr) {
                if (auto unknown = unknownKey(path + "." + member.key); !unknown) {
                    return unknown;
                }
                continue;
            }
            if (*slot != nullptr) {
                return fail("serialization.duplicate_field", "'" + member.key + "' is given twice",
                            path + "." + member.key);
            }
            *slot = &member.value;
        }
        if (id == nullptr) {
            return fail("level.invalid_id", "an entity record needs an 'id', its stable id", path);
        }
        const Expected<Uuid> parsed =
            id->kind() == Value::Kind::String ? Uuid::parse(id->asString()) : Expected<Uuid>(Uuid{});
        if (!parsed || parsed->isNil()) {
            return fail("level.invalid_id", "an entity's id is a UUID other than nil", path + ".id");
        }
        record.id = *parsed;
        if (!m_ids.emplace(record.id, static_cast<std::uint32_t>(index)).second) {
            return fail("level.duplicate_id", "two entities have the id " + record.id.toString(), path + ".id");
        }
        for (const Value* section : {record.components, record.editor}) {
            if (section != nullptr && section->kind() != Value::Kind::Object) {
                return fail("serialization.wrong_type",
                            "a record's 'components' and 'editor' are objects keyed by type name",
                            path + "." + std::string(section == record.components ? kComponentsKey : kEditorKey));
            }
        }
        return {};
    }

    // Reads every component of a record: into a value, or kept as unresolved.
    Expected<void, serialization::Error> readComponents(std::size_t index) {
        const LoadedRecord& record = m_records[index];
        m_names.clear();
        m_types.clear();
        for (const bool editor : {false, true}) {
            const Value* section = editor ? record.editor : record.components;
            if (section == nullptr) {
                continue;
            }
            for (const Value::Member& member : section->members()) {
                if (auto component = readComponent(index, editor, member); !component) {
                    return component;
                }
            }
        }
        return {};
    }

    Expected<void, serialization::Error> readComponent(std::size_t index, bool editor, const Value::Member& member) {
        LoadedRecord& record = m_records[index];
        const std::string path = componentPath(index, editor, member.key);
        if (!m_names.insert(member.key).second) {
            return fail("serialization.duplicate_field", "the component " + member.key + " is given twice", path);
        }
        const TypeInfo* type = m_registry.find(member.key);
        if ((type != nullptr && type->id == typeOf<StableId>().id) ||
            (type == nullptr && member.key == typeOf<StableId>().name)) {
            return fail("level.invalid_id", "the stable id is the record's 'id', not a component", path);
        }
        if (type == nullptr || type->kind != TypeKind::Struct || type->role != TypeRole::Component ||
            isTransient(*type)) {
            const std::string why = type == nullptr ? "is unknown" : "isn't a saved component type";
            return unresolved(record, editor, member, path, "the component type " + member.key + " " + why);
        }
        if (!m_types.insert(type->id).second) {
            return fail("serialization.duplicate_field",
                        "the component " + std::string(type->name) + " is given twice, also under its FormerName",
                        path);
        }

        LoadContext context{.records = &m_ids,
                            .referrer = &record.id,
                            .typeName = type->name,
                            .childOf = type->id == typeOf<ChildOf>().id,
                            .path = path,
                            .source = m_options.source};
        const serialization::EntityMapping mapping{.toEntity = &toPlaceholder, .context = &context};
        std::vector<serialization::Error> valueWarnings;
        OwnedValue value(*type);
        const Expected<void, serialization::Error> read =
            serialization::fromValue(member.value, *type, value.get(),
                                     {.unknownFields = m_options.unknownFields,
                                      .warnings = &valueWarnings,
                                      .allocationBudget = m_budget,
                                      .entities = &mapping});
        if (!read) {
            if (failsUnknown()) {
                return makeUnexpected(placed(read.error(), path, m_options.source));
            }
            return unresolved(record, editor, member, path,
                              "the value of " + member.key + " can't be read (" + read.error().message + " at " +
                                  read.error().path + ")");
        }
        for (serialization::Error& warning : valueWarnings) {
            m_warnings.push_back(placed(std::move(warning), path, m_options.source));
        }
        for (serialization::Error& warning : context.warnings) {
            m_warnings.push_back(std::move(warning));
        }
        if (context.childOf) {
            const Entity parent = value.as<ChildOf>()->parent;
            if (parent.isNull()) {
                m_warnings.push_back(makeError("level.dropped_parent",
                                               "entity " + record.id.toString() +
                                                   ": its parent is nil or no entity of the level; it loads as a root",
                                               m_options.source, path));
                return {};
            }
            m_parents[index] = parent.index();
            record.childOf = std::move(value);
            return {};
        }
        record.values.push_back(std::move(value));
        return {};
    }

    // Keeps a component that can't be read, with a warning, or fails with UnknownFields::Fail.
    Expected<void, serialization::Error> unresolved(LoadedRecord& record, bool editor, const Value::Member& member,
                                                    const std::string& path, const std::string& why) {
        if (failsUnknown()) {
            return fail("level.unresolved_component", why, path);
        }
        m_warnings.push_back(
            makeError("level.unresolved_component",
                      "entity " + record.id.toString() + ": " + why + "; its value is kept and saved back",
                      m_options.source, path));
        record.unresolved.push_back({.typeName = member.key, .editor = editor, .value = member.value});
        return {};
    }

    // Fails when the parents form a cycle, an entity being its own parent included.
    Expected<void, serialization::Error> checkCycles() const {
        enum class Mark : std::uint8_t {
            None,
            OnPath,
            Done
        };
        std::vector<Mark> marks(m_records.size(), Mark::None);
        std::vector<std::uint32_t> path;
        for (std::uint32_t start = 0; start < m_records.size(); ++start) {
            path.clear();
            std::uint32_t at = start;
            while (at != kNoRecord && marks[at] == Mark::None) {
                marks[at] = Mark::OnPath;
                path.push_back(at);
                at = m_parents[at];
            }
            if (at != kNoRecord && marks[at] == Mark::OnPath) {
                return fail("level.parent_cycle",
                            "entity " + m_records[at].id.toString() +
                                (m_parents[at] == at ? " is its own parent" : " is its own ancestor"),
                            componentPath(at, false, typeOf<ChildOf>().name));
            }
            for (const std::uint32_t visited : path) {
                marks[visited] = Mark::Done;
            }
        }
        return {};
    }

    const TypeRegistry& m_registry;
    const LevelLoadOptions& m_options;
    std::size_t m_budget = 0;
    std::vector<LoadedRecord> m_records;
    std::unordered_map<Uuid, std::uint32_t> m_ids;
    std::vector<std::uint32_t> m_parents; // the record of each record's parent, or kNoRecord
    std::vector<serialization::Error> m_warnings;
    // The names and types of the record being read, for duplicates.
    std::unordered_set<std::string_view> m_names;
    std::unordered_set<std::uint64_t> m_types;
};

} // namespace

Expected<LevelInfo, serialization::Error> loadLevel(std::string_view text, World& world, const TypeRegistry& registry,
                                                    const LevelLoadOptions& options) {
    ITH_ASSERT_MSG(!world.isIterating(), "level: loadLevel while the world is iterated or a system or hook runs");
    if (world.isIterating()) {
        return makeUnexpected(worldBusy(options.source));
    }
    const std::size_t budget = serialization::allocationBudget(text.size(), options.allocationBudget);
    std::vector<serialization::Error> documentWarnings;
    Uuid levelId;
    const Expected<Value, serialization::Error> data =
        serialization::readDocument(text, kLevelDocument,
                                    {.source = options.source,
                                     .unknownFields = options.unknownFields,
                                     .warnings = &documentWarnings,
                                     .allocationBudget = budget,
                                     .id = &levelId});
    if (!data) {
        return makeUnexpected(data.error());
    }
    LevelReader reader(registry, options, budget);
    if (const Expected<void, serialization::Error> read = reader.read(*data); !read) {
        return makeUnexpected(read.error());
    }
    reader.build(world);
    if (options.warnings != nullptr) {
        for (std::vector<serialization::Error>* list : {&documentWarnings, &reader.warnings()}) {
            options.warnings->insert(options.warnings->end(), std::make_move_iterator(list->begin()),
                                     std::make_move_iterator(list->end()));
        }
    }
    return LevelInfo{.id = levelId, .entityCount = reader.recordCount()};
}

} // namespace ith
