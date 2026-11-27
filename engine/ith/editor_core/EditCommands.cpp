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
#include "ith/editor_core/CommandArgs.h"
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {

namespace {

using detail::applyAll;
using detail::checkEditable;
using detail::childrenOf;
using detail::componentType;
using detail::defaultValueAt;
using detail::idValue;
using detail::invalidArgs;
using detail::liveEntity;
using detail::nextSibling;
using detail::optionalId;
using detail::optionalString;
using detail::outlinerOrder;
using detail::parseId;
using detail::placeholder;
using detail::readValue;
using detail::requiredString;
using detail::stableIdOf;
using detail::targets;
using serialization::Value;

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

// The "before" of attach and detach: nullopt when missing, the null entity (the end) when null,
// else a live sibling under `parent` (null: a root) that isn't one of `moved`.
Expected<std::optional<Entity>> placement(const EditorSession& session, std::string_view command, const Value& args,
                                          Entity parent, const std::vector<Entity>& moved) {
    const Value* given = args.kind() == Value::Kind::Object ? args.find("before") : nullptr;
    if (given == nullptr) {
        return std::optional<Entity>();
    }
    Expected<std::optional<Uuid>> id = optionalId(command, args, "before");
    if (!id) {
        return makeUnexpected(std::move(id).error());
    }
    if (!id->has_value()) {
        return std::optional<Entity>(Entity());
    }
    Expected<Entity> before = liveEntity(session, command, **id);
    if (!before) {
        return makeUnexpected(std::move(before).error());
    }
    const auto* link = session.document().world().get<const ChildOf>(*before);
    const Entity beforeParent = link != nullptr ? link->parent : Entity();
    if (beforeParent != parent || std::ranges::any_of(moved, [&](Entity entity) { return entity == *before; })) {
        return invalidArgs(command, "\"before\" must be a sibling at the new place that isn't moved");
    }
    return std::optional<Entity>(*before);
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
    const World& world = session.document().world();
    Expected<std::optional<Entity>> before = placement(session, kCommand, args, *parent, *entities);
    if (!before) {
        return makeUnexpected(std::move(before).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        // A child of the parent already keeps its link, and its despawnWithParent, and only moves.
        const auto* link = world.get<const ChildOf>(entity);
        const bool relink = link == nullptr || link->parent != *parent;
        if (!relink && !before->has_value()) {
            continue;
        }
        EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = entity, .placeBefore = *before});
        if (relink) {
            patch.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = *parent})));
        }
    }
    return applyAll(session, patches);
}

Expected<Value> detach(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.detach";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    const World& world = session.document().world();
    Expected<std::optional<Entity>> before = placement(session, kCommand, args, Entity(), *entities);
    if (!before) {
        return makeUnexpected(std::move(before).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        const bool linked = world.has<ChildOf>(entity);
        if (!linked && !before->has_value()) {
            continue;
        }
        EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = entity, .placeBefore = *before});
        if (linked) {
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

// The "path" of a property command: a component path of a component type commands may edit,
// refused when a field along it is ReadOnly.
Expected<PropertyPath> editablePath(const EditorSession& session, std::string_view command, const Value& args) {
    Expected<std::string_view> text = requiredString(command, args, "path");
    if (!text) {
        return makeUnexpected(std::move(text).error());
    }
    Expected<PropertyPath> path = PropertyPath::parseQualified(session.types(), *text);
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    if (Expected<void> editable = checkEditable(session, command, *path); !editable) {
        return makeUnexpected(std::move(editable).error());
    }
    return path;
}

// One Patch change per entity, its entries added by `build`.
template <typename Build>
std::vector<EntityPatch> patchEach(const std::vector<Entity>& entities, const PropertyPath& path, const Build& build) {
    std::vector<EntityPatch> patches;
    for (const Entity entity : entities) {
        Patch patch(path.rootType());
        build(patch);
        EntityPatch& entityPatch = patches.emplace_back(EntityPatch{.entity = entity});
        entityPatch.changes.push_back(ComponentChange::makePatch(std::move(patch)));
    }
    return patches;
}

Expected<Value> setProperty(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.set_property";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<PropertyPath> path = editablePath(session, kCommand, args);
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    const Value* json = args.find("value");
    if (json == nullptr) {
        return invalidArgs(kCommand, "\"value\" is required");
    }
    Expected<OwnedValue> value = readValue(session, path->leafType(), json);
    if (!value) {
        return makeUnexpected(std::move(value).error());
    }
    return applyAll(session, patchEach(*entities, *path, [&](Patch& patch) { patch.set(*path, *value); }));
}

// Reset to Default: the value a default-constructed component has at the path; an element or an
// optional's value that the default doesn't have resets to its type's default.
Expected<Value> resetProperty(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.reset_property";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<PropertyPath> path = editablePath(session, kCommand, args);
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    const OwnedValue value = defaultValueAt(*path);
    return applyAll(session, patchEach(*entities, *path, [&](Patch& patch) { patch.set(*path, value); }));
}

// The "index" member: nullopt when missing, an error when not a non-negative integer.
Expected<std::optional<std::size_t>> optionalIndex(std::string_view command, const Value& args) {
    const Value* index = args.kind() == Value::Kind::Object ? args.find("index") : nullptr;
    if (index == nullptr || index->kind() == Value::Kind::Null) {
        return std::optional<std::size_t>();
    }
    const std::optional<std::uint64_t> number =
        index->kind() == Value::Kind::Number ? index->toNumber<std::uint64_t>() : std::nullopt;
    if (!number.has_value()) {
        return invalidArgs(command, "\"index\" must be a non-negative integer");
    }
    return std::optional<std::size_t>(static_cast<std::size_t>(*number));
}

// The path of element `index` of the vector at `vector`.
Expected<PropertyPath> elementPath(const EditorSession& session, const PropertyPath& vector, std::size_t index) {
    return PropertyPath::parseQualified(session.types(),
                                        vector.toQualifiedString() + "[" + std::to_string(index) + "]");
}

// The size of the vector at `path` in the entity's component; 0 when the entity has no such
// component (the patch then fails) or the path doesn't resolve.
std::size_t vectorSize(const World& world, Entity entity, const PropertyPath& path) {
    const void* component = world.get(entity, path.rootType());
    if (component == nullptr) {
        return 0;
    }
    const Expected<const void*> vector = resolve(path, component);
    return vector ? path.leafType().container->size(*vector) : 0;
}

// Insert Element and Erase Element: "path" names a std::vector.
Expected<PropertyPath> vectorPath(const EditorSession& session, std::string_view command, const Value& args) {
    Expected<PropertyPath> path = editablePath(session, command, args);
    if (path && path->leafType().kind != TypeKind::DynamicArray) {
        return invalidArgs(command, "\"path\" must name a std::vector");
    }
    return path;
}

Expected<Value> insertElement(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.insert_element";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<PropertyPath> path = vectorPath(session, kCommand, args);
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    Expected<std::optional<std::size_t>> index = optionalIndex(kCommand, args);
    if (!index) {
        return makeUnexpected(std::move(index).error());
    }
    Expected<OwnedValue> value = readValue(session, path->leafType().element(), args.find("value"));
    if (!value) {
        return makeUnexpected(std::move(value).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        Expected<PropertyPath> element =
            elementPath(session, *path, index->value_or(vectorSize(session.document().world(), entity, *path)));
        if (!element) {
            return makeUnexpected(std::move(element).error());
        }
        Patch patch(path->rootType());
        patch.insert(std::move(*element), *value);
        EntityPatch& entityPatch = patches.emplace_back(EntityPatch{.entity = entity});
        entityPatch.changes.push_back(ComponentChange::makePatch(std::move(patch)));
    }
    return applyAll(session, patches);
}

Expected<Value> eraseElement(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.erase_element";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    Expected<PropertyPath> path = vectorPath(session, kCommand, args);
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    Expected<std::optional<std::size_t>> index = optionalIndex(kCommand, args);
    if (!index) {
        return makeUnexpected(std::move(index).error());
    }
    if (!index->has_value()) {
        return invalidArgs(kCommand, "\"index\" is required");
    }
    Expected<PropertyPath> element = elementPath(session, *path, **index);
    if (!element) {
        return makeUnexpected(std::move(element).error());
    }
    return applyAll(session, patchEach(*entities, *path, [&](Patch& patch) { patch.erase(*element); }));
}

// Enabled: removes or adds the Disabled tag (ADR-0031 rule 9); entities already so are left alone.
Expected<Value> setEnabled(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.entity.set_enabled";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    const Value* enabled = args.kind() == Value::Kind::Object ? args.find("enabled") : nullptr;
    if (enabled == nullptr || enabled->kind() != Value::Kind::Bool) {
        return invalidArgs(kCommand, "\"enabled\" must be true or false");
    }
    const World& world = session.document().world();
    std::vector<EntityPatch> patches;
    for (const Entity entity : *entities) {
        if (world.has<Disabled>(entity) != enabled->asBool()) {
            continue;
        }
        EntityPatch& patch = patches.emplace_back(EntityPatch{.entity = entity});
        patch.changes.push_back(enabled->asBool() ? ComponentChange::makeRemove(typeOf<Disabled>())
                                                  : ComponentChange::makeAdd(OwnedValue::of(Disabled{})));
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

// The copies of a Duplicate, by the bits of each original.
using CopyTable = std::unordered_map<std::uint64_t, Entity>;

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMap::function has
Entity toCopy(void* context, Entity entity) {
    const auto& copies = *static_cast<const CopyTable*>(context);
    const auto found = copies.find(entity.bits());
    return found != copies.end() ? found->second : entity;
}

// Duplicate, as the editor duplicates actors: a copy of each target, with a new stable id, placed
// right after it; the selection becomes the copies. Children that aren't targets are not copied.
// References between the targets (a selected child of a selected parent) point at the copies, so
// selecting a parent and its child duplicates that hierarchy; references to other entities stay.
// Invariant: this function must not run registry commands. A gizmo Alt drag (ViewportGizmo.cpp) calls
// it directly inside its open transaction, outside the registry's depth counter, where the registry
// would refuse a nested command as an open interaction.
Expected<Value> duplicate(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.edit.duplicate";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    const World& world = session.document().world();
    // In Outliner order, so a parent's copy is spawned before its child's.
    std::vector<Entity> order;
    for (const Entity entity : outlinerOrder(world)) {
        if (std::ranges::any_of(*entities, [entity](Entity target) { return target == entity; })) {
            order.push_back(entity);
        }
    }
    CopyTable copies;
    for (const Entity entity : order) {
        copies.emplace(entity.bits(), placeholder(static_cast<std::uint32_t>(copies.size())));
    }
    const EntityMap map{.function = &toCopy, .context = &copies};
    std::vector<EntityPatch> patches;
    std::vector<Uuid> ids;
    Value result = Value::object();
    Value list = Value::array();
    for (const Entity entity : order) {
        const Uuid id = Uuid::generate();
        const auto* link = world.get<const ChildOf>(entity);
        const bool parentCopied = link != nullptr && copies.contains(link->parent.bits());
        EntityPatch& spawn =
            patches.emplace_back(EntityPatch{.op = EntityOp::Spawn, .entity = copies.at(entity.bits())});
        if (!parentCopied) {
            spawn.placeBefore = nextSibling(world, entity);
        }
        spawn.changes.push_back(ComponentChange::makeAdd(OwnedValue::of(StableId{.id = id})));
        world.forEachComponent(entity, [&](const TypeInfo& type, const void* value) {
            if (type.id == typeOf<StableId>().id || type.id == typeOf<Children>().id ||
                type.id == typeOf<DisabledInHierarchy>().id) {
                return;
            }
            OwnedValue copy = OwnedValue::copyOf(type, value);
            remapEntities(type, copy.get(), map);
            spawn.changes.push_back(ComponentChange::makeAdd(std::move(copy)));
        });
        ids.push_back(id);
        list.elements().push_back(idValue(id));
    }
    Expected<Value> applied = applyAll(session, patches);
    if (!applied) {
        return applied;
    }
    setSelection(session, ids);
    result.set("ids", std::move(list));
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
    registry.add(
        {.id = "ith.entity.attach",
         .displayName = "Attach To",
         .tooltip =
             "Make the entities children of a parent, keeping where they are in the world; optionally before a sibling",
         .category = "Entity",
         .execute = &attach});
    registry.add({.id = "ith.entity.detach",
                  .displayName = "Detach",
                  .tooltip = "Make the entities roots, keeping where they are in the world; optionally before a root",
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
    registry.add({.id = "ith.entity.reset_property",
                  .displayName = "Reset to Default",
                  .tooltip = "Set a value of a component of the entities back to its default",
                  .category = "Entity",
                  .execute = &resetProperty});
    registry.add({.id = "ith.entity.insert_element",
                  .displayName = "Add Element",
                  .tooltip = "Insert an element into a list of a component of the entities",
                  .category = "Entity",
                  .execute = &insertElement});
    registry.add({.id = "ith.entity.erase_element",
                  .displayName = "Remove Element",
                  .tooltip = "Remove an element from a list of a component of the entities",
                  .category = "Entity",
                  .execute = &eraseElement});
    registry.add({.id = "ith.entity.set_enabled",
                  .displayName = "Set Enabled",
                  .tooltip = "Enable or disable the entities and everything below them",
                  .category = "Entity",
                  .execute = &setEnabled});
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
