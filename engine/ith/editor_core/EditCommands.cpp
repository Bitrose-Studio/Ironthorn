// The built-in Edit, Entity and Selection commands of editor_core (ADR-0037 rule 4).
//
// Arguments name entities by stable id ("ids", "id", "parent", "before") and components by fully
// qualified type name ("type", the keys of "components"); values are JSON read through fromValue
// with the session's entity mapping. Commands that take "ids" use the selection when it is left
// out. Every command validates its arguments before it changes anything, and an Edit command
// changes the world through one World::applyPatches inside the transaction the registry opened, so
// a failure changes nothing.

#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/math/Transform.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/TextArchive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {

namespace {

using serialization::Value;

Unexpected<Error> invalidArgs(std::string_view command, std::string message) {
    return makeError("editor.command.invalid_args", std::string(command) + ": " + std::move(message));
}

Unexpected<Error> fromSerialization(const serialization::Error& error) {
    std::string message = error.message;
    if (!error.path.empty()) {
        message += " (at " + error.path + ")";
    }
    return makeError(error.code, std::move(message));
}

// The string member `key` of `args`; nullopt when it is missing or null.
Expected<std::optional<std::string_view>> optionalString(std::string_view command, const Value& args,
                                                         std::string_view key) {
    const Value* value = args.kind() == Value::Kind::Object ? args.find(key) : nullptr;
    if (value == nullptr || value->kind() == Value::Kind::Null) {
        return std::optional<std::string_view>();
    }
    if (value->kind() != Value::Kind::String) {
        return invalidArgs(command, "\"" + std::string(key) + "\" must be a string");
    }
    return std::optional<std::string_view>(value->asString());
}

Expected<std::string_view> requiredString(std::string_view command, const Value& args, std::string_view key) {
    Expected<std::optional<std::string_view>> text = optionalString(command, args, key);
    if (!text) {
        return makeUnexpected(std::move(text).error());
    }
    if (!text->has_value()) {
        return invalidArgs(command, "\"" + std::string(key) + "\" is required");
    }
    return **text;
}

Expected<Uuid> parseId(std::string_view command, std::string_view text) {
    Expected<Uuid> id = Uuid::parse(text);
    if (!id || id->isNil()) {
        return invalidArgs(command, "\"" + std::string(text) + "\" is not a stable id");
    }
    return *id;
}

// The live entity whose stable id is `id`.
Expected<Entity> liveEntity(const EditorSession& session, std::string_view command, const Uuid& id) {
    const Entity entity = session.document().world().findByStableId(id);
    if (entity.isNull()) {
        return makeError("editor.entity.not_found", std::string(command) + ": no entity has the id " + id.toString());
    }
    return entity;
}

// The stable id member `key`; nullopt when missing or null.
Expected<std::optional<Uuid>> optionalId(std::string_view command, const Value& args, std::string_view key) {
    Expected<std::optional<std::string_view>> text = optionalString(command, args, key);
    if (!text) {
        return makeUnexpected(std::move(text).error());
    }
    if (!text->has_value()) {
        return std::optional<Uuid>();
    }
    Expected<Uuid> id = parseId(command, **text);
    if (!id) {
        return makeUnexpected(std::move(id).error());
    }
    return std::optional<Uuid>(*id);
}

// The entities of "ids", or the selection when "ids" is missing; at least one, each live.
Expected<std::vector<Entity>> targets(const EditorSession& session, std::string_view command, const Value& args) {
    std::vector<Uuid> ids;
    const Value* list = args.kind() == Value::Kind::Object ? args.find("ids") : nullptr;
    if (list == nullptr || list->kind() == Value::Kind::Null) {
        ids = session.document().selection().entities();
    } else {
        if (list->kind() != Value::Kind::Array) {
            return invalidArgs(command, "\"ids\" must be an array of stable ids");
        }
        for (const Value& element : list->elements()) {
            if (element.kind() != Value::Kind::String) {
                return invalidArgs(command, "\"ids\" must be an array of stable ids");
            }
            Expected<Uuid> id = parseId(command, element.asString());
            if (!id) {
                return makeUnexpected(std::move(id).error());
            }
            ids.push_back(*id);
        }
    }
    if (ids.empty()) {
        return invalidArgs(command, "no entities given and nothing selected");
    }
    std::vector<Entity> entities;
    for (const Uuid& id : ids) {
        Expected<Entity> entity = liveEntity(session, command, id);
        if (!entity) {
            return makeUnexpected(std::move(entity).error());
        }
        if (std::ranges::none_of(entities, [&](Entity other) { return other == *entity; })) {
            entities.push_back(*entity);
        }
    }
    return entities;
}

// A component type of the session's registry that commands may add, remove or edit.
Expected<const TypeInfo*> componentType(const EditorSession& session, std::string_view command, std::string_view name) {
    const TypeInfo* type = session.types().find(name);
    if (type == nullptr || type->role != TypeRole::Component) {
        return invalidArgs(command, "\"" + std::string(name) + "\" is not a component type");
    }
    if (type->id == typeOf<StableId>().id || type->id == typeOf<Children>().id ||
        type->id == typeOf<DisabledInHierarchy>().id) {
        return invalidArgs(command, std::string(name) + " is kept by the editor and the World");
    }
    return type;
}

// `json` read as a value of `type`; a missing value is the type's default.
Expected<OwnedValue> readValue(const EditorSession& session, const TypeInfo& type, const Value* json) {
    OwnedValue value(type);
    if (json == nullptr || json->kind() == Value::Kind::Null) {
        return value;
    }
    const serialization::ValueReadOptions options{.unknownFields = serialization::UnknownFields::Fail,
                                                  .entities = &session.entityMapping()};
    if (auto read = serialization::fromValue(*json, type, value.get(), options); !read) {
        return fromSerialization(read.error());
    }
    return value;
}

Value idValue(const Uuid& id) {
    return Value::string(id.toString());
}

Uuid stableIdOf(const World& world, Entity entity) {
    const auto* id = world.get<const StableId>(entity);
    return id != nullptr ? id->id : Uuid();
}

const std::vector<Entity>* childrenOf(const World& world, Entity entity) {
    const auto* children = world.get<const Children>(entity);
    return children != nullptr ? &children->entities : nullptr;
}

// Every entity in Outliner order: each root, then its subtree depth-first.
void outlinerOrder(const World& world, Entity entity, std::vector<Entity>& order) {
    order.push_back(entity);
    if (const std::vector<Entity>* children = childrenOf(world, entity)) {
        for (const Entity child : *children) {
            outlinerOrder(world, child, order);
        }
    }
}

std::vector<Entity> outlinerOrder(const World& world) {
    std::vector<Entity> order;
    world.forEachRoot([&](Entity root) { outlinerOrder(world, root, order); });
    return order;
}

// The sibling after `entity` under its parent, or among the roots; null for the last.
Entity nextSibling(const World& world, Entity entity) {
    std::vector<Entity> siblings;
    if (const auto* link = world.get<const ChildOf>(entity)) {
        if (const std::vector<Entity>* children = childrenOf(world, link->parent)) {
            siblings = *children;
        }
    } else {
        world.forEachRoot([&](Entity root) { siblings.push_back(root); });
    }
    const auto found = std::ranges::find_if(siblings, [entity](Entity sibling) { return sibling == entity; });
    return found == siblings.end() || found + 1 == siblings.end() ? Entity() : *(found + 1);
}

// Applies one set of patches in the open transaction.
Expected<Value> applyAll(EditorSession& session, std::span<const EntityPatch> patches, Value result = {}) {
    // Joins the transaction the registry opened for the command.
    TransactionScope scope = session.document().transactions().begin("");
    Expected<void> applied = scope.apply(patches);
    scope.commit();
    if (!applied) {
        return makeUnexpected(std::move(applied).error());
    }
    return result;
}

Entity placeholder(std::uint32_t index) {
    return Entity::fromParts(index, 1, 0);
}

// --- Undo and redo ---

Expected<void> notPlaying(EditorSession& session) {
    if (session.document().playing()) {
        return makeError("editor.command.playing", "nothing is undoable while playing");
    }
    return {};
}

void addUndoCommands(CommandRegistry& registry) {
    registry.add({
        .id = "ith.edit.undo",
        .displayName = "Undo",
        .tooltip = "Undo the last change",
        .category = "Edit",
        .kind = CommandKind::Action,
        .canExecute = [](EditorSession& session) -> Expected<void> {
            if (Expected<void> allowed = notPlaying(session); !allowed) {
                return allowed;
            }
            if (!session.document().history().canUndo()) {
                return makeError("editor.undo.nothing", "there is nothing to undo");
            }
            return {};
        },
        .execute = [](EditorSession& session, const Value&) -> Expected<Value> {
            if (Expected<void> undone = session.document().transactions().undo(); !undone) {
                return makeUnexpected(std::move(undone).error());
            }
            return Value();
        },
    });
    registry.add({
        .id = "ith.edit.redo",
        .displayName = "Redo",
        .tooltip = "Redo the last undone change",
        .category = "Edit",
        .kind = CommandKind::Action,
        .canExecute = [](EditorSession& session) -> Expected<void> {
            if (Expected<void> allowed = notPlaying(session); !allowed) {
                return allowed;
            }
            if (!session.document().history().canRedo()) {
                return makeError("editor.redo.nothing", "there is nothing to redo");
            }
            return {};
        },
        .execute = [](EditorSession& session, const Value&) -> Expected<Value> {
            if (Expected<void> redone = session.document().transactions().redo(); !redone) {
                return makeUnexpected(std::move(redone).error());
            }
            return Value();
        },
    });
}

// --- Selection ---

void setSelection(EditorSession& session, std::span<const Uuid> ids) {
    if (session.document().selection().replace(ids)) {
        session.document().changes().bumpSelection();
    }
}

// Applies one selection mode to the selection; returns whether it changed.
Expected<bool> changeSelection(Selection& selection, std::string_view how, std::span<const Uuid> ids) {
    if (how != "replace" && how != "add" && how != "toggle" && how != "remove") {
        return invalidArgs("ith.selection.set", R"("mode" must be replace, add, toggle or remove)");
    }
    if (how == "replace") {
        return selection.replace(ids);
    }
    bool changed = false;
    for (const Uuid& id : ids) {
        bool one = false;
        if (how == "add") {
            one = selection.add(id);
        } else if (how == "toggle") {
            one = selection.toggle(id);
        } else {
            one = selection.remove(id);
        }
        changed = changed || one;
    }
    return changed;
}

Expected<Value> setSelectionCommand(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.selection.set";
    const Value* list = args.kind() == Value::Kind::Object ? args.find("entities") : nullptr;
    if (list == nullptr || list->kind() != Value::Kind::Array) {
        return invalidArgs(kCommand, R"("entities" must be an array of stable ids)");
    }
    std::vector<Uuid> ids;
    for (const Value& element : list->elements()) {
        if (element.kind() != Value::Kind::String) {
            return invalidArgs(kCommand, R"("entities" must be an array of stable ids)");
        }
        Expected<Uuid> id = parseId(kCommand, element.asString());
        if (!id) {
            return makeUnexpected(std::move(id).error());
        }
        if (Expected<Entity> entity = liveEntity(session, kCommand, *id); !entity) {
            return makeUnexpected(std::move(entity).error());
        }
        ids.push_back(*id);
    }
    Expected<std::optional<std::string_view>> mode = optionalString(kCommand, args, "mode");
    if (!mode) {
        return makeUnexpected(std::move(mode).error());
    }
    Expected<bool> changed = changeSelection(session.document().selection(), mode->value_or("replace"), ids);
    if (!changed) {
        return makeUnexpected(std::move(changed).error());
    }
    if (*changed) {
        session.document().changes().bumpSelection();
    }
    return Value();
}

void addSelectionCommands(CommandRegistry& registry) {
    registry.add({
        .id = "ith.selection.set",
        .displayName = "Select",
        .tooltip = "Change the selection: replace it, or add, toggle or remove entities",
        .category = "Edit",
        .kind = CommandKind::Action,
        .execute = &setSelectionCommand,
    });
    registry.add({
        .id = "ith.edit.select_all",
        .displayName = "Select All",
        .tooltip = "Select every entity of the level",
        .category = "Edit",
        .kind = CommandKind::Action,
        .execute = [](EditorSession& session, const Value&) -> Expected<Value> {
            const World& world = session.document().world();
            std::vector<Uuid> ids;
            for (const Entity entity : outlinerOrder(world)) {
                if (const Uuid id = stableIdOf(world, entity); !id.isNil()) {
                    ids.push_back(id);
                }
            }
            setSelection(session, ids);
            return Value();
        },
    });
    registry.add({
        .id = "ith.edit.select_none",
        .displayName = "Select None",
        .tooltip = "Clear the selection",
        .category = "Edit",
        .kind = CommandKind::Action,
        .execute = [](EditorSession& session, const Value&) -> Expected<Value> {
            setSelection(session, {});
            return Value();
        },
    });
}

// --- Entities ---

Expected<Value> createEntity(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.create";
    const World& world = session.document().world();
    Expected<std::optional<Uuid>> given = optionalId(kCommand, args, "id");
    if (!given) {
        return makeUnexpected(std::move(given).error());
    }
    const Uuid id = given->value_or(Uuid::generate());
    if (!world.findByStableId(id).isNull()) {
        return invalidArgs(kCommand, "an entity already has the id " + id.toString());
    }
    Expected<std::optional<std::string_view>> label = optionalString(kCommand, args, "label");
    if (!label) {
        return makeUnexpected(std::move(label).error());
    }
    EntityPatch spawn{.op = EntityOp::Spawn, .entity = placeholder(0)};
    spawn.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(StableId{.id = id})));
    spawn.changes.push_back(
        ComponentChange::makeAdd(OwnedValue::of(EditorLabel{.name = std::string(label->value_or("Entity"))})));
    bool hasTransform = false;
    if (const Value* components = args.kind() == Value::Kind::Object ? args.find("components") : nullptr;
        components != nullptr && components->kind() != Value::Kind::Null) {
        if (components->kind() != Value::Kind::Object) {
            return invalidArgs(kCommand, "\"components\" must be an object of type names and values");
        }
        for (const Value::Member& member : components->members()) {
            Expected<const TypeInfo*> type = componentType(session, kCommand, member.key);
            if (!type) {
                return makeUnexpected(std::move(type).error());
            }
            if ((*type)->id == typeOf<ChildOf>().id || (*type)->id == typeOf<EditorLabel>().id) {
                return invalidArgs(kCommand, R"(give the parent and the label as "parent" and "label")");
            }
            Expected<OwnedValue> value = readValue(session, **type, &member.value);
            if (!value) {
                return makeUnexpected(std::move(value).error());
            }
            hasTransform = hasTransform || (*type)->id == typeOf<LocalTransform>().id;
            spawn.changes.push_back(ComponentChange::makeAdd(std::move(*value)));
        }
    }
    // An Actor: an entity with a transform (ADR-0008 rule 3).
    if (!hasTransform) {
        spawn.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(LocalTransform{})));
    }
    spawn.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(WorldTransform{})));
    Expected<std::optional<Uuid>> parent = optionalId(kCommand, args, "parent");
    if (!parent) {
        return makeUnexpected(std::move(parent).error());
    }
    if (parent->has_value()) {
        Expected<Entity> parentEntity = liveEntity(session, kCommand, **parent);
        if (!parentEntity) {
            return makeUnexpected(std::move(parentEntity).error());
        }
        spawn.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = *parentEntity})));
    }
    Expected<Value> created = applyAll(session, std::span(&spawn, 1));
    if (!created) {
        return created;
    }
    const std::array<Uuid, 1> selected{id};
    setSelection(session, selected);
    Value result = Value::object();
    result.set("id", idValue(id));
    return result;
}

Expected<Value> rename(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.edit.rename";
    Expected<std::optional<Uuid>> id = optionalId(kCommand, args, "id");
    if (!id) {
        return makeUnexpected(std::move(id).error());
    }
    const std::optional<Uuid> target = id->has_value() ? *id : session.document().selection().primary();
    if (!target.has_value()) {
        return invalidArgs(kCommand, "no entity given and nothing selected");
    }
    Expected<Entity> entity = liveEntity(session, kCommand, *target);
    if (!entity) {
        return makeUnexpected(std::move(entity).error());
    }
    Expected<std::string_view> label = requiredString(kCommand, args, "label");
    if (!label) {
        return makeUnexpected(std::move(label).error());
    }
    EntityPatch patch{.entity = *entity};
    patch.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(EditorLabel{.name = std::string(*label)})));
    return applyAll(session, std::span(&patch, 1));
}

Expected<Value> attach(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.attach";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<std::optional<Uuid>> parentId = optionalId(kCommand, args, "parent");
    if (!parentId) {
        return makeUnexpected(std::move(parentId).error());
    }
    if (!parentId->has_value()) {
        return invalidArgs(kCommand, "\"parent\" is required");
    }
    Expected<Entity> parent = liveEntity(session, kCommand, **parentId);
    if (!parent) {
        return makeUnexpected(std::move(parent).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = entity});
        patch.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = *parent})));
    }
    return applyAll(session, patches);
}

Expected<Value> detach(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.detach";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        if (session.document().world().has<ChildOf>(entity)) {
            EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = entity});
            patch.changes.push_back(ComponentChange::makeRemove(typeOf<ChildOf>()));
        }
    }
    return applyAll(session, patches);
}

Expected<Value> moveBefore(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.move_before";
    Expected<std::optional<Uuid>> id = optionalId(kCommand, args, "id");
    if (!id) {
        return makeUnexpected(std::move(id).error());
    }
    if (!id->has_value()) {
        return invalidArgs(kCommand, "\"id\" is required");
    }
    Expected<Entity> entity = liveEntity(session, kCommand, **id);
    if (!entity) {
        return makeUnexpected(std::move(entity).error());
    }
    Expected<std::optional<Uuid>> beforeId = optionalId(kCommand, args, "before");
    if (!beforeId) {
        return makeUnexpected(std::move(beforeId).error());
    }
    Entity before;
    if (beforeId->has_value()) {
        Expected<Entity> sibling = liveEntity(session, kCommand, **beforeId);
        if (!sibling) {
            return makeUnexpected(std::move(sibling).error());
        }
        before = *sibling;
    }
    const EntityPatch patch{.entity = *entity, .placeBefore = before};
    return applyAll(session, std::span(&patch, 1));
}

Expected<Value> addComponent(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.add_component";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<std::string_view> name = requiredString(kCommand, args, "type");
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    Expected<const TypeInfo*> type = componentType(session, kCommand, *name);
    if (!type) {
        return makeUnexpected(std::move(type).error());
    }
    Expected<OwnedValue> value = readValue(session, **type, args.find("value"));
    if (!value) {
        return makeUnexpected(std::move(value).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = entity});
        patch.changes.push_back(ComponentChange::makeAdd(*value));
    }
    return applyAll(session, patches);
}

Expected<Value> removeComponent(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.remove_component";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<std::string_view> name = requiredString(kCommand, args, "type");
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    Expected<const TypeInfo*> type = componentType(session, kCommand, *name);
    if (!type) {
        return makeUnexpected(std::move(type).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = entity});
        patch.changes.push_back(ComponentChange::makeRemove(**type));
    }
    return applyAll(session, patches);
}

Expected<Value> setProperty(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.set_property";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<std::string_view> text = requiredString(kCommand, args, "path");
    if (!text) {
        return makeUnexpected(std::move(text).error());
    }
    Expected<PropertyPath> path = PropertyPath::parseQualified(session.types(), *text);
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    if (Expected<const TypeInfo*> type = componentType(session, kCommand, path->rootType().name); !type) {
        return makeUnexpected(std::move(type).error());
    }
    // ADR-0033 rule 10: patches don't check ReadOnly; the command layer does.
    if (hasFlags(path->chainFlags(), ReflectionFlags::ReadOnly)) {
        return makeError("editor.command.read_only",
                         std::string(kCommand) + ": " + std::string(*text) + " is read-only");
    }
    const Value* json = args.find("value");
    if (json == nullptr) {
        return invalidArgs(kCommand, "\"value\" is required");
    }
    Expected<OwnedValue> value = readValue(session, path->leafType(), json);
    if (!value) {
        return makeUnexpected(std::move(value).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        Patch patch(path->rootType());
        patch.set(*path, *value);
        EntityPatch& entityPatch = patches.emplace_back(EntityPatch{.entity = entity});
        entityPatch.changes.push_back(ComponentChange::makePatch(std::move(patch)));
    }
    return applyAll(session, patches);
}

// Delete and Delete with children (ADR-0031 rule 7): the targets go, with their children marked
// despawnWithParent (all of them for `everything`); other children are detached in place first.
Expected<Value> deleteEntities(EditorSession& session, const Value& args, bool everything, std::string_view command) {
    Expected<std::vector<Entity>> entities = targets(session, command, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    const World& world = session.document().world();
    std::unordered_set<std::uint64_t> doomed;
    std::vector<Entity> pending = *entities;
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        if (!doomed.insert(entity.bits()).second) {
            continue;
        }
        if (const std::vector<Entity>* children = childrenOf(world, entity)) {
            for (const Entity child : *children) {
                if (everything || world.get<const ChildOf>(child)->despawnWithParent) {
                    pending.push_back(child);
                }
            }
        }
    }
    std::vector<EntityPatch> patches;
    std::vector<Entity> order;
    for (const Entity entity : outlinerOrder(world)) {
        if (!doomed.contains(entity.bits())) {
            continue;
        }
        order.push_back(entity);
        if (const std::vector<Entity>* children = childrenOf(world, entity)) {
            for (const Entity child : *children) {
                if (!doomed.contains(child.bits())) {
                    EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = child});
                    patch.changes.push_back(ComponentChange::makeRemove(typeOf<ChildOf>()));
                }
            }
        }
    }
    // Children before their parents: the reverse of Outliner order.
    for (const Entity entity : std::views::reverse(order)) {
        patches.push_back(EntityPatch{.op = EntityOp::Despawn, .entity = entity});
    }
    return applyAll(session, patches);
}

// Duplicate: a copy of each target, with a new stable id, placed right after it; the selection
// becomes the copies.
Expected<Value> duplicate(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.edit.duplicate";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    const World& world = session.document().world();
    std::vector<EntityPatch> patches;
    std::vector<Uuid> copies;
    Value ids = Value::array();
    for (const Entity entity : *entities) {
        const Uuid id = Uuid::generate();
        EntityPatch& spawn =
            patches.emplace_back(EntityPatch{.op = EntityOp::Spawn,
                                             .entity = placeholder(static_cast<std::uint32_t>(copies.size())),
                                             .placeBefore = nextSibling(world, entity)});
        spawn.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(StableId{.id = id})));
        world.forEachComponent(entity, [&](const TypeInfo& type, const void* value) {
            if (type.id != typeOf<StableId>().id && type.id != typeOf<Children>().id &&
                type.id != typeOf<DisabledInHierarchy>().id) {
                spawn.changes.push_back(ComponentChange::makeAdd(OwnedValue::copyOf(type, value)));
            }
        });
        copies.push_back(id);
        ids.elements().push_back(idValue(id));
    }
    Expected<Value> applied = applyAll(session, patches);
    if (!applied) {
        return applied;
    }
    setSelection(session, copies);
    Value result = Value::object();
    result.set("ids", std::move(ids));
    return result;
}

void addEntityCommands(CommandRegistry& registry) {
    registry.add({.id = "ith.entity.create",
                  .displayName = "Create Entity",
                  .tooltip = "Create an entity with a transform; it becomes the selection",
                  .category = "Entity",
                  .execute = &createEntity});
    registry.add({.id = "ith.edit.rename",
                  .displayName = "Rename",
                  .tooltip = "Change the name the Outliner shows",
                  .category = "Edit",
                  .execute = &rename});
    registry.add({.id = "ith.entity.attach",
                  .displayName = "Attach To",
                  .tooltip = "Make the entities children of a parent, keeping where they are in the world",
                  .category = "Entity",
                  .execute = &attach});
    registry.add({.id = "ith.entity.detach",
                  .displayName = "Detach",
                  .tooltip = "Make the entities roots, keeping where they are in the world",
                  .category = "Entity",
                  .execute = &detach});
    registry.add({.id = "ith.entity.move_before",
                  .displayName = "Move",
                  .tooltip = "Move an entity before a sibling in the Outliner, or to the end",
                  .category = "Entity",
                  .execute = &moveBefore});
    registry.add({.id = "ith.entity.add_component",
                  .displayName = "Add Component",
                  .tooltip = "Add a component, or replace the one the entities have",
                  .category = "Entity",
                  .execute = &addComponent});
    registry.add({.id = "ith.entity.remove_component",
                  .displayName = "Remove Component",
                  .tooltip = "Remove a component from the entities",
                  .category = "Entity",
                  .execute = &removeComponent});
    registry.add({.id = "ith.entity.set_property",
                  .displayName = "Set Property",
                  .tooltip = "Set a value of a component of the entities",
                  .category = "Entity",
                  .execute = &setProperty});
    registry.add({.id = "ith.edit.delete",
                  .displayName = "Delete",
                  .tooltip = "Delete the entities; their other children stay where they are",
                  .category = "Edit",
                  .execute = [](EditorSession& session, const Value& args) {
                      return deleteEntities(session, args, false, "ith.edit.delete");
                  }});
    registry.add({.id = "ith.edit.delete_with_children",
                  .displayName = "Delete with Children",
                  .tooltip = "Delete the entities and everything below them",
                  .category = "Edit",
                  .execute = [](EditorSession& session, const Value& args) {
                      return deleteEntities(session, args, true, "ith.edit.delete_with_children");
                  }});
    registry.add({.id = "ith.edit.duplicate",
                  .displayName = "Duplicate",
                  .tooltip = "Copy the entities next to themselves; the copies become the selection",
                  .category = "Edit",
                  .execute = &duplicate});
}

} // namespace

void registerEditCommands(CommandRegistry& registry) {
    addUndoCommands(registry);
    addSelectionCommands(registry);
    addEntityCommands(registry);
}

} // namespace ith
