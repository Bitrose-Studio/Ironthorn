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
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {
namespace {

using serialization::printable;
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

// The path of a component; the type name comes from the data, so it is made printable.
std::string componentPath(std::size_t record, bool editor, std::string_view typeName) {
    return recordPath(record) + "." + std::string(editor ? kEditorKey : kComponentsKey) + "." + printable(typeName);
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

// A type whose values a level loads as components: a component struct that is saved.
bool isLevelComponent(const TypeInfo& type) {
    return type.kind == TypeKind::Struct && type.role == TypeRole::Component && !isTransient(type);
}

serialization::Error worldBusy(std::string_view source) {
    return makeError("level.world_busy",
                     "the world is being iterated, or a system or hook runs on it; levels are saved and loaded "
                     "between frames",
                     source);
}

// The warnings of a save or load: none at all when the caller doesn't collect them, and at most
// kMaxPerCode of each code, then one per code that counts the rest. accept() tells whether a
// warning is kept before it is built, so dropped warnings cost a count only.
class Warnings {
public:
    static constexpr std::size_t kMaxPerCode = kMaxLevelWarningsPerCode;

    explicit Warnings(bool collect) : m_collect(collect) {}

    [[nodiscard]] bool collects() const noexcept { return m_collect; }

    // Counts a warning of `code`; true when it is kept, so the caller builds and adds it.
    bool accept(std::string_view code) {
        if (!m_collect) {
            return false;
        }
        auto found = m_counts.find(code);
        if (found == m_counts.end()) {
            found = m_counts.emplace(std::string(code), 0).first;
        }
        return ++found->second <= kMaxPerCode;
    }

    void add(serialization::Error warning) { m_kept.push_back(std::move(warning)); }

    // Adds a warning built elsewhere (by readDocument or fromValue), if it is kept.
    void offer(serialization::Error warning) {
        if (accept(warning.code)) {
            add(std::move(warning));
        }
    }

    // Appends the kept warnings, then a count of the dropped ones per code, to `out`.
    void moveTo(std::vector<serialization::Error>& out, std::string_view source) {
        out.insert(out.end(), std::make_move_iterator(m_kept.begin()), std::make_move_iterator(m_kept.end()));
        for (const auto& [code, count] : m_counts) {
            if (count > kMaxPerCode) {
                out.push_back(makeError(
                    code, std::to_string(count - kMaxPerCode) + " more " + code + " warnings are dropped", source));
            }
        }
        m_kept.clear();
        m_counts.clear();
    }

private:
    bool m_collect;
    std::vector<serialization::Error> m_kept;
    std::map<std::string, std::size_t, std::less<>> m_counts; // ordered, so the summaries are too
};

// ---------------------------------------------------------------------------------------------
// Saving

struct SaveContext {
    const World* world = nullptr;
    const Uuid* referrer = nullptr; // the stable id of the entity being written
    std::string_view typeName;      // the component being written
    std::string path;               // its place in the document
    Warnings* warnings = nullptr;
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
    if (save.warnings->accept("level.unsaved_reference")) {
        save.warnings->add(makeError("level.unsaved_reference",
                                     "entity " + save.referrer->toString() + ": " + std::string(save.typeName) +
                                         " refers to an entity that is not saved (no StableId, stale, or of "
                                         "another world); the reference is saved as the nil id",
                                     {}, save.path));
    }
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
// then the entries of its UnresolvedComponents that the next load won't refuse (see
// keepsUnresolved).
class RecordWriter {
public:
    RecordWriter(const World& world, const TypeRegistry& registry, SaveContext& context)
        : m_world(world), m_registry(registry), m_context(context) {}

    Expected<void, serialization::Error> collect(Entity entity, bool parentSaved, std::size_t recordIndex,
                                                 std::vector<SavedComponent>& components) {
        const serialization::EntityMapping mapping{.toId = &toStableId, .context = &m_context};
        m_values.clear();
        m_world.forEachComponent(
            entity, [this](const TypeInfo& type, const void* value) { m_values.emplace_back(&type, value); });
        components.clear();
        m_types.clear();
        for (const auto& [type, value] : m_values) {
            if (isTransient(*type) || type->id == typeOf<StableId>().id) {
                continue;
            }
            if (type->id == typeOf<ChildOf>().id && !parentSaved) {
                if (m_context.warnings->accept("level.dropped_parent")) {
                    m_context.warnings->add(makeError("level.dropped_parent",
                                                      "entity " + m_context.referrer->toString() +
                                                          ": its parent has no StableId and is not saved; it is "
                                                          "saved as a root",
                                                      {}, recordPath(recordIndex)));
                }
                continue;
            }
            m_context.typeName = type->name;
            m_context.path = componentPath(recordIndex, isEditorOnly(*type), type->name);
            Expected<Value, serialization::Error> written = serialization::toValue(*type, value, &mapping);
            if (!written) {
                return makeUnexpected(placed(std::move(written).error(), m_context.path, {}));
            }
            m_types.insert(type->id);
            components.push_back({.typeName = type->name, .editor = isEditorOnly(*type), .value = std::move(*written)});
        }
        if (const auto* unresolved = m_world.get<const UnresolvedComponents>(entity)) {
            for (const UnresolvedComponent& entry : unresolved->entries) {
                if (keepsUnresolved(entry, components)) {
                    components.push_back({.typeName = entry.typeName, .editor = entry.editor, .value = entry.value});
                }
            }
        }
        // Bytewise: std::char_traits<char> compares as unsigned char.
        std::ranges::sort(components,
                          [](const SavedComponent& a, const SavedComponent& b) { return a.typeName < b.typeName; });
        return {};
    }

private:
    // Whether an unresolved entry is written. Loading refuses a record that names one component type
    // twice (also through a FormerName) or has a StableId component, so an entry is left out when the
    // registry now resolves its name to StableId, or to a component type that a real component or
    // an earlier entry of the record already has. An entry with the name of a written component is
    // left out too, whatever the registry says.
    bool keepsUnresolved(const UnresolvedComponent& entry, const std::vector<SavedComponent>& components) {
        if (std::ranges::any_of(components,
                                [&entry](const SavedComponent& saved) { return saved.typeName == entry.typeName; })) {
            return false;
        }
        const TypeInfo* type = m_registry.find(entry.typeName);
        if (type == nullptr) {
            return true;
        }
        if (type->id == typeOf<StableId>().id) {
            return false;
        }
        return !isLevelComponent(*type) || m_types.insert(type->id).second;
    }

    const World& m_world;
    const TypeRegistry& m_registry;
    SaveContext& m_context;
    std::vector<std::pair<const TypeInfo*, const void*>> m_values;
    std::unordered_set<std::uint64_t> m_types; // the component types the record writes
};

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
                                                      const TypeRegistry& registry, const LevelSaveOptions& options) {
    ITH_ASSERT_MSG(!world.isIterating(), "level: saveLevel while the world is iterated or a system or hook runs");
    if (world.isIterating()) {
        return makeUnexpected(worldBusy({}));
    }
    if (levelId.isNil()) {
        return makeUnexpected(makeError("serialization.invalid_id", "a level's id can't be the nil id", {}));
    }

    Warnings warnings(options.warnings != nullptr);
    SaveContext context{.world = &world, .warnings = &warnings};
    RecordWriter writer(world, registry, context);

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
            writer.collect(next.entity, next.parentSaved, entities.elements().size(), components);
        if (!collected) {
            return makeUnexpected(collected.error());
        }
        entities.elements().push_back(makeRecord(stableId->id, components));
    }
    if (unsavedCount > 0 && warnings.accept("level.unsaved_entity")) {
        warnings.add(makeError("level.unsaved_entity",
                               std::to_string(unsavedCount) + (unsavedCount == 1 ? " entity has" : " entities have") +
                                   " no StableId and " + (unsavedCount == 1 ? "is" : "are") + " not saved",
                               {}));
    }

    Value data = Value::object();
    data.members().push_back({.key = std::string(kEntitiesKey), .value = std::move(entities)});
    Expected<std::string, serialization::Error> text = serialization::writeDocument(kLevelDocument, data, &levelId);
    if (text && options.warnings != nullptr) {
        warnings.moveTo(*options.warnings, {});
    }
    return text;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Loading

constexpr std::uint32_t kNoRecord = std::numeric_limits<std::uint32_t>::max();

// While a document is read, an Entity value holds a placeholder: the index of the record it names,
// with generation 1 under world id 0. No World hands out world id 0, which only the null handle
// has, so a placeholder can't be taken for an entity of a real world. Building turns each
// placeholder into the entity spawned for that record.
constexpr std::uint32_t kPlaceholderGeneration = 1;

Entity placeholder(std::uint32_t record) {
    return Entity::fromParts(record, kPlaceholderGeneration, 0);
}

// What the bytes kept per record cost against the allocation budget, beyond its components: the
// record itself, its id's entry in the lookup and its parent.
constexpr std::size_t kRecordCost = sizeof(Uuid) + (sizeof(void*) * 12) + 64;

struct LoadContext {
    const std::unordered_map<Uuid, std::uint32_t>* records = nullptr;
    const Uuid* referrer = nullptr; // the record being read
    std::string_view typeName;      // the component being read
    bool childOf = false;           // ChildOf reports an unknown parent as a dropped parent instead
    std::string path;               // the component's place in the document
    std::string_view source;
    Warnings* warnings = nullptr;
    std::vector<serialization::Error> pending; // of the component being read, kept only when it reads
};

// The entity mapping of a load: an id names the record that has it, as a placeholder.
Entity toPlaceholder(void* context, const Uuid& id) {
    LoadContext& load = *static_cast<LoadContext*>(context);
    if (const auto found = load.records->find(id); found != load.records->end()) {
        return placeholder(found->second);
    }
    if (!load.childOf && load.warnings->collects()) {
        load.pending.push_back(makeError("level.unknown_reference",
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
    const bool valid =
        entity.worldId() == 0 && entity.generation() == kPlaceholderGeneration && entity.index() < spawned.size();
    ITH_ASSERT_MSG(valid, "level: an entity value that is no placeholder of this load");
    return valid ? spawned[entity.index()] : Entity{};
}

// One record, read and checked.
struct LoadedRecord {
    Uuid id;
    Value* components = nullptr;
    Value* editor = nullptr;
    OwnedValue childOf;             // empty without a (kept) parent
    std::vector<OwnedValue> values; // the other components, in document order
    std::vector<UnresolvedComponent> unresolved;
};

class LevelReader {
public:
    LevelReader(const TypeRegistry& registry, const LevelLoadOptions& options, std::size_t budget)
        : m_registry(registry), m_options(options), m_budget(budget), m_warnings(options.warnings != nullptr) {}

    // Reads and checks the document's data. On success the records are ready to build. Unresolved
    // values are moved out of `data`.
    Expected<void, serialization::Error> read(Value& data) {
        if (data.kind() != Value::Kind::Object) {
            return fail("serialization.wrong_type", "a level's data is an object with 'entities'", "$");
        }
        Value* entities = nullptr;
        for (Value::Member& member : data.members()) {
            if (member.key == kEntitiesKey) {
                if (entities != nullptr) {
                    return fail("serialization.duplicate_field", "'entities' is given twice", "$.entities");
                }
                entities = &member.value;
            } else if (auto unknown = unknownKey("$." + printable(member.key)); !unknown) {
                return unknown;
            }
        }
        if (entities == nullptr) {
            return {}; // a missing field keeps its default: no entities
        }
        if (entities->kind() != Value::Kind::Array) {
            return fail("serialization.wrong_type", "'entities' is an array of entity records", "$.entities");
        }
        const std::size_t count = entities->elements().size();
        if (count >= kNoRecord || count > m_budget / kRecordCost) {
            return tooLarge("$.entities");
        }
        m_budget -= count * kRecordCost;

        m_records.resize(count);
        m_ids.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            if (auto shape = readShape(entities->elements()[index], index); !shape) {
                return shape;
            }
        }
        m_parents.assign(count, kNoRecord);
        for (std::size_t index = 0; index < count; ++index) {
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
            // The parent first: a LocalTransform added after it keeps its saved value. checkCycles
            // proved the hierarchy acyclic, so the World needn't walk the ancestors of each link,
            // which would make a deep hierarchy quadratic.
            if (record.childOf.hasValue()) {
                [[maybe_unused]] const void* added =
                    detail::addAcyclic(world, entity, *record.childOf.type(), record.childOf.get());
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

    [[nodiscard]] Warnings& warnings() noexcept { return m_warnings; }

private:
    Unexpected<serialization::Error> fail(std::string code, std::string message, std::string path) const {
        return makeUnexpected(makeError(std::move(code), std::move(message), m_options.source, std::move(path)));
    }

    Unexpected<serialization::Error> tooLarge(std::string path) const {
        return fail("serialization.too_large", "the level needs more memory than the allocation budget allows",
                    std::move(path));
    }

    // Takes `bytes` from the budget; false, taking nothing, when that is more than is left.
    bool charge(std::size_t bytes) noexcept {
        if (bytes > m_budget) {
            return false;
        }
        m_budget -= bytes;
        return true;
    }

    [[nodiscard]] bool failsUnknown() const noexcept {
        return m_options.unknownFields == serialization::UnknownFields::Fail;
    }

    // An unknown key of the data or of a record: a warning, or an error with UnknownFields::Fail.
    // `path` is already printable.
    Expected<void, serialization::Error> unknownKey(const std::string& path) {
        if (failsUnknown()) {
            return fail("serialization.unknown_field", "unknown field", path);
        }
        if (m_warnings.accept("serialization.unknown_field")) {
            m_warnings.add(
                makeError("serialization.unknown_field", "unknown field; it is dropped", m_options.source, path));
        }
        return {};
    }

    // Checks a record's keys and id, and notes its sections.
    Expected<void, serialization::Error> readShape(Value& value, std::size_t index) {
        const std::string path = recordPath(index);
        if (value.kind() != Value::Kind::Object) {
            return fail("serialization.wrong_type", "an entity record is an object with 'id' and 'components'", path);
        }
        LoadedRecord& record = m_records[index];
        const Value* id = nullptr;
        for (Value::Member& member : value.members()) {
            if (member.key == kIdKey) {
                if (id != nullptr) {
                    return fail("serialization.duplicate_field", "'id' is given twice", path + ".id");
                }
                id = &member.value;
                continue;
            }
            Value** section = nullptr;
            if (member.key == kComponentsKey) {
                section = &record.components;
            } else if (member.key == kEditorKey) {
                section = &record.editor;
            }
            if (section == nullptr) {
                if (auto unknown = unknownKey(path + "." + printable(member.key)); !unknown) {
                    return unknown;
                }
                continue;
            }
            if (*section != nullptr) {
                return fail("serialization.duplicate_field", "'" + member.key + "' is given twice",
                            path + "." + member.key);
            }
            *section = &member.value;
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
            Value* section = editor ? record.editor : record.components;
            if (section == nullptr) {
                continue;
            }
            for (Value::Member& member : section->members()) {
                if (auto component = readComponent(index, editor, member); !component) {
                    return component;
                }
            }
        }
        return {};
    }

    Expected<void, serialization::Error> readComponent(std::size_t index, bool editor, Value::Member& member) {
        LoadedRecord& record = m_records[index];
        const std::string path = componentPath(index, editor, member.key);
        const std::string name = printable(member.key);
        if (!m_names.insert(member.key).second) {
            return fail("serialization.duplicate_field", "the component " + name + " is given twice", path);
        }
        const TypeInfo* type = m_registry.find(member.key);
        if ((type != nullptr && type->id == typeOf<StableId>().id) ||
            (type == nullptr && member.key == typeOf<StableId>().name)) {
            return fail("level.invalid_id", "the stable id is the record's 'id', not a component", path);
        }
        if (type == nullptr || !isLevelComponent(*type)) {
            const char* why = type == nullptr ? " is unknown" : " isn't a saved component type";
            return unresolved(record, editor, member, path, [&] { return "the component type " + name + why; });
        }
        if (!m_types.insert(type->id).second) {
            return fail("serialization.duplicate_field",
                        "the component " + std::string(type->name) + " is given twice, also under its FormerName",
                        path);
        }
        if (!charge(type->size + sizeof(OwnedValue))) {
            return tooLarge(path);
        }

        LoadContext context{.records = &m_ids,
                            .referrer = &record.id,
                            .typeName = type->name,
                            .childOf = type->id == typeOf<ChildOf>().id,
                            .path = path,
                            .source = m_options.source,
                            .warnings = &m_warnings};
        const serialization::EntityMapping mapping{.toEntity = &toPlaceholder, .context = &context};
        std::vector<serialization::Error> valueWarnings;
        OwnedValue value(*type);
        const Expected<void, serialization::Error> read =
            serialization::fromValue(member.value, *type, value.get(),
                                     {.unknownFields = m_options.unknownFields,
                                      .warnings = m_warnings.collects() ? &valueWarnings : nullptr,
                                      .sharedBudget = &m_budget,
                                      .entities = &mapping});
        if (!read) {
            if (failsUnknown() || read.error().code == "serialization.too_large") {
                return makeUnexpected(placed(read.error(), path, m_options.source));
            }
            return unresolved(record, editor, member, path, [&] {
                return "the value of " + name + " can't be read (" + read.error().message + " at " + read.error().path +
                       ")";
            });
        }
        for (serialization::Error& warning : valueWarnings) {
            m_warnings.offer(placed(std::move(warning), path, m_options.source));
        }
        for (serialization::Error& warning : context.pending) {
            m_warnings.offer(std::move(warning));
        }
        if (context.childOf) {
            const Entity parent = value.as<ChildOf>()->parent;
            if (parent.isNull()) {
                if (m_warnings.accept("level.dropped_parent")) {
                    m_warnings.add(makeError("level.dropped_parent",
                                             "entity " + record.id.toString() +
                                                 ": its parent is nil or no entity of the level; it loads as a root",
                                             m_options.source, path));
                }
                return {};
            }
            m_parents[index] = parent.index();
            record.childOf = std::move(value);
            return {};
        }
        record.values.push_back(std::move(value));
        return {};
    }

    // Keeps a component that can't be read, with a warning, or fails with UnknownFields::Fail. The
    // raw value is moved out of the document, so keeping it allocates only its name. `why` builds
    // the message, only when it is needed.
    template <typename Why>
    Expected<void, serialization::Error> unresolved(LoadedRecord& record, bool editor, Value::Member& member,
                                                    const std::string& path, const Why& why) {
        if (failsUnknown()) {
            return fail("level.unresolved_component", why(), path);
        }
        if (!charge(member.key.size() + sizeof(UnresolvedComponent))) {
            return tooLarge(path);
        }
        if (m_warnings.accept("level.unresolved_component")) {
            m_warnings.add(
                makeError("level.unresolved_component",
                          "entity " + record.id.toString() + ": " + why() + "; its value is kept and saved back",
                          m_options.source, path));
        }
        record.unresolved.push_back({.typeName = member.key, .editor = editor, .value = std::move(member.value)});
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
    std::size_t m_budget = 0; // the bytes the values read from the tree may still take
    Warnings m_warnings;
    std::vector<LoadedRecord> m_records;
    std::unordered_map<Uuid, std::uint32_t> m_ids;
    std::vector<std::uint32_t> m_parents; // the record of each record's parent, or kNoRecord
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
    Expected<Value, serialization::Error> data =
        serialization::readDocument(text, kLevelDocument,
                                    {.source = options.source,
                                     .unknownFields = options.unknownFields,
                                     .warnings = options.warnings != nullptr ? &documentWarnings : nullptr,
                                     .allocationBudget = budget,
                                     .id = &levelId});
    if (!data) {
        return makeUnexpected(std::move(data).error());
    }
    LevelReader reader(registry, options, budget);
    for (serialization::Error& warning : documentWarnings) {
        reader.warnings().offer(std::move(warning));
    }
    if (const Expected<void, serialization::Error> read = reader.read(*data); !read) {
        return makeUnexpected(read.error());
    }
    // Building can't fail, so the warnings are handed over first: from the clear on, nothing the
    // caller passed is touched (see the aliasing precondition in Level.h).
    if (options.warnings != nullptr) {
        reader.warnings().moveTo(*options.warnings, options.source);
    }
    const LevelInfo info{.id = levelId, .entityCount = reader.recordCount()};
    reader.build(world);
    return info;
}

} // namespace ith
