// The built-in Query commands (ADR-0037 rule 17): read-only commands for automation, tests and,
// later, the MCP adapter. Entities are named by stable id; component values are JSON as toValue
// writes them, with entities as stable ids.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/foundation/Hash.h"
#include "ith/level/Level.h"
#include "ith/serialization/TextArchive.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ith {

namespace {

using detail::childrenOf;
using detail::fromSerialization;
using detail::idValue;
using detail::invalidArgs;
using detail::liveEntity;
using detail::optionalId;
using detail::outlinerOrder;
using detail::requiredString;
using detail::stableIdOf;
using serialization::Value;

Value idOrNull(const Uuid& id) {
    return id.isNil() ? Value() : idValue(id);
}

Value errorValue(const Error& error) {
    Value value = Value::object();
    value.set("code", Value::string(error.code));
    value.set("message", Value::string(error.message));
    return value;
}

Expected<Value> documentQuery(EditorSession& session, const Value& /*args*/) {
    const EditorDocument& document = session.document();
    Value result = Value::object();
    result.set("path", Value::string(document.path()));
    result.set("levelId", idValue(document.levelId()));
    result.set("dirty", Value::boolean(document.dirty()));
    result.set("saving", Value::boolean(document.saving()));
    result.set("opening", Value::boolean(document.opening()));
    result.set("playing", Value::boolean(document.playing()));
    result.set("entities", Value::unsignedInteger(document.world().entityCount()));
    result.set("lastError", session.lastFileError() ? errorValue(*session.lastFileError()) : Value());
    return result;
}

Expected<Value> selectionQuery(EditorSession& session, const Value& /*args*/) {
    const Selection& selection = session.document().selection();
    Value entities = Value::array();
    for (const Uuid& id : selection.entities()) {
        entities.elements().push_back(idValue(id));
    }
    Value result = Value::object();
    result.set("entities", std::move(entities));
    result.set("primary", selection.primary() ? idValue(*selection.primary()) : Value());
    return result;
}

Expected<Value> entityQuery(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.query.entity";
    Expected<std::optional<Uuid>> id = optionalId(kCommand, args, "id");
    if (!id) {
        return makeUnexpected(std::move(id).error());
    }
    if (!id->has_value()) {
        return invalidArgs(kCommand, R"("id" is required)");
    }
    Expected<Entity> entity = liveEntity(session, kCommand, **id);
    if (!entity) {
        return makeUnexpected(std::move(entity).error());
    }
    const World& world = session.document().world();
    Value result = Value::object();
    result.set("id", idValue(**id));
    const auto* label = world.get<const EditorLabel>(*entity);
    result.set("label", label != nullptr ? Value::string(label->name) : Value());
    const auto* link = world.get<const ChildOf>(*entity);
    result.set("parent", link != nullptr ? idOrNull(stableIdOf(world, link->parent)) : Value());
    Value children = Value::array();
    if (const std::vector<Entity>* list = childrenOf(world, *entity)) {
        for (const Entity child : *list) {
            children.elements().push_back(idOrNull(stableIdOf(world, child)));
        }
    }
    result.set("children", std::move(children));
    Value components = Value::object();
    std::optional<Error> failed;
    world.forEachComponent(*entity, [&](const TypeInfo& type, const void* value) {
        if (failed || hasFlags(type.flags, ReflectionFlags::Transient) || type.id == typeOf<StableId>().id) {
            return;
        }
        Expected<Value, serialization::Error> json = serialization::toValue(type, value, &session.entityMapping());
        if (!json) {
            failed = fromSerialization(json.error()).error();
            return;
        }
        components.set(std::string(type.name), std::move(*json));
    });
    if (failed) {
        return makeUnexpected(std::move(*failed));
    }
    std::ranges::sort(components.members(), {},
                      [](const Value::Member& member) { return std::string_view(member.key); });
    result.set("components", std::move(components));
    return result;
}

Expected<Value> findQuery(EditorSession& session, const Value& args) {
    Expected<std::string_view> label = requiredString("ith.query.find", args, "label");
    if (!label) {
        return makeUnexpected(std::move(label).error());
    }
    const World& world = session.document().world();
    Value ids = Value::array();
    for (const Entity entity : outlinerOrder(world)) {
        const auto* name = world.get<const EditorLabel>(entity);
        if (name != nullptr && name->name == *label) {
            ids.elements().push_back(idOrNull(stableIdOf(world, entity)));
        }
    }
    Value result = Value::object();
    result.set("ids", std::move(ids));
    return result;
}

Expected<Value> historyQuery(EditorSession& session, const Value& /*args*/) {
    const UndoHistory& history = session.document().history();
    Value entries = Value::array();
    for (const UndoEntry& entry : history.entries()) {
        entries.elements().push_back(Value::string(entry.name));
    }
    Value result = Value::object();
    result.set("entries", std::move(entries));
    result.set("cursor", Value::unsignedInteger(history.cursor()));
    result.set("canUndo", Value::boolean(history.canUndo()));
    result.set("canRedo", Value::boolean(history.canRedo()));
    return result;
}

// A hash of the level text, which is the same for the same world (ADR-0034 rule 9).
Expected<Value> worldHashQuery(EditorSession& session, const Value& /*args*/) {
    Expected<std::string, serialization::Error> text =
        saveLevel(session.document().world(), session.document().levelId(), session.types());
    if (!text) {
        return fromSerialization(text.error());
    }
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string hex(16, '0');
    std::uint64_t hash = fnv1a64(*text);
    for (auto digit = hex.rbegin(); digit != hex.rend(); ++digit, hash >>= 4U) {
        *digit = kDigits[hash & 15U];
    }
    Value result = Value::object();
    result.set("hash", Value::string(std::move(hex)));
    result.set("entities", Value::unsignedInteger(session.document().world().entityCount()));
    return result;
}

void addQuery(CommandRegistry& registry, std::string id, std::string displayName, std::string tooltip,
              Expected<Value> (*execute)(EditorSession&, const Value&)) {
    registry.add({.id = std::move(id),
                  .displayName = std::move(displayName),
                  .tooltip = std::move(tooltip),
                  .category = "Query",
                  .kind = CommandKind::Query,
                  .execute = execute});
}

} // namespace

void registerQueryCommands(CommandRegistry& registry) {
    addQuery(registry, "ith.query.document", "Document", "The open level: path, id, dirty, saving, opening, playing",
             &documentQuery);
    addQuery(registry, "ith.query.selection", "Selection", "The selected entities and the primary one",
             &selectionQuery);
    addQuery(registry, "ith.query.entity", "Entity", "An entity's label, parent, children and component values",
             &entityQuery);
    addQuery(registry, "ith.query.find", "Find", "The entities with a label, in Outliner order", &findQuery);
    addQuery(registry, "ith.query.history", "Undo History", "The undo entries and how many are undoable",
             &historyQuery);
    addQuery(registry, "ith.query.world_hash", "World Hash", "A hash of the level as it would be saved",
             &worldHashQuery);
}

} // namespace ith
