#include "ith/editor_core/CommandArgs.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Transactions.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/TextArchive.h"

#include <algorithm>
#include <string>
#include <utility>

namespace ith::detail {

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

namespace {

void appendSubtree(const World& world, Entity entity, std::vector<Entity>& order) {
    order.push_back(entity);
    if (const std::vector<Entity>* children = childrenOf(world, entity)) {
        for (const Entity child : *children) {
            appendSubtree(world, child, order);
        }
    }
}

} // namespace

std::vector<Entity> outlinerOrder(const World& world) {
    std::vector<Entity> order;
    world.forEachRoot([&](Entity root) { appendSubtree(world, root, order); });
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
Expected<Value> applyAll(EditorSession& session, std::span<const EntityPatch> patches, Value result) {
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

} // namespace ith::detail
