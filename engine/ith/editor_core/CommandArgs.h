#pragma once

// Helpers of the built-in commands (EditCommands.cpp, FileCommands.cpp, QueryCommands.cpp): reading
// arguments, naming entities by stable id, and changing the world in the command's transaction.
// Internal to editor_core.

#include "ith/ecs/EntityPatch.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/Value.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ith {

class EditorSession;
class World;
class PropertyPath;
struct TypeInfo;

namespace detail {

// "editor.command.invalid_args" for `command`.
[[nodiscard]] Unexpected<Error> invalidArgs(std::string_view command, std::string message);

// A serialization error as an engine error, its path appended to the message.
[[nodiscard]] Unexpected<Error> fromSerialization(const serialization::Error& error);

// The string member `key` of `args`: nullopt when missing or null, an error when not a string.
[[nodiscard]] Expected<std::optional<std::string_view>>
optionalString(std::string_view command, const serialization::Value& args, std::string_view key);
[[nodiscard]] Expected<std::string_view> requiredString(std::string_view command, const serialization::Value& args,
                                                        std::string_view key);

// A stable id from text; nil ids are refused.
[[nodiscard]] Expected<Uuid> parseId(std::string_view command, std::string_view text);
[[nodiscard]] Expected<std::optional<Uuid>> optionalId(std::string_view command, const serialization::Value& args,
                                                       std::string_view key);

// The live entity of the active world (EditorSession::active) with the stable id, or
// "editor.entity.not_found".
[[nodiscard]] Expected<Entity> liveEntity(EditorSession& session, std::string_view command, const Uuid& id);

// The entities of "ids", or the active selection when it is missing; at least one, each live in the
// active world, no repeats.
[[nodiscard]] Expected<std::vector<Entity>> targets(EditorSession& session, std::string_view command,
                                                    const serialization::Value& args);

// The mapping that writes an entity of `world` as its stable id (the nil id when it has none or
// belongs to another world) and reads an id as `world`'s live entity with it. `world` must outlive it.
[[nodiscard]] serialization::EntityMapping stableIdMapping(const World& world);

// The Outliner label of `entity` of `world`: its EditorLabel, or, in a game world, which has none,
// the label of the editor world's entity with the same stable id (ADR-0053 decision 15); null when
// neither has one. The pointer is into a component of `world` or of the editor world: valid only
// until either world's structure or that label changes, so copy it before running a command.
[[nodiscard]] const std::string* labelOf(const EditorSession& session, const World& world, Entity entity);

// A component type of the session's registry that commands may add, remove or edit (not StableId,
// Children or DisabledInHierarchy).
[[nodiscard]] Expected<const TypeInfo*> componentType(const EditorSession& session, std::string_view command,
                                                      std::string_view name);

// `json` read as a value of `type` with unknown fields refused, entities through the active world's
// mapping; null or missing: the default.
[[nodiscard]] Expected<OwnedValue> readValue(EditorSession& session, const TypeInfo& type,
                                             const serialization::Value* json);

[[nodiscard]] serialization::Value idValue(const Uuid& id);

// The entity's stable id; nil when it has none.
[[nodiscard]] Uuid stableIdOf(const World& world, Entity entity);

[[nodiscard]] const std::vector<Entity>* childrenOf(const World& world, Entity entity);

// Whether an ancestor of `entity` is in `set` (entity handles' bits).
[[nodiscard]] bool hasAncestorIn(const World& world, Entity entity, const std::unordered_set<std::uint64_t>& set);

// Every entity in Outliner order: each root, then its subtree depth-first.
[[nodiscard]] std::vector<Entity> outlinerOrder(const World& world);

// The sibling after `entity` under its parent or among the roots; null for the last.
[[nodiscard]] Entity nextSibling(const World& world, Entity entity);

// Applies `patches` in the transaction the registry opened for the command, in the active world;
// returns `result`.
[[nodiscard]] Expected<serialization::Value> applyAll(EditorSession& session, std::span<const EntityPatch> patches,
                                                      serialization::Value result = {});

// Refuses edits at `path`: a component type commands may not edit, or a ReadOnly field along it
// ("editor.command.read_only"; ADR-0033 rule 10: patches do not check ReadOnly, the command layer
// does).
[[nodiscard]] Expected<void> checkEditable(const EditorSession& session, std::string_view command,
                                           const PropertyPath& path);

// The value a default-constructed component of path.rootType() has at `path`; an element or an
// optional's value the default does not have is its type's default (Reset to Default).
[[nodiscard]] OwnedValue defaultValueAt(const PropertyPath& path);

// The Spawn placeholder `index` (EntityPatch.h).
[[nodiscard]] Entity placeholder(std::uint32_t index);

} // namespace detail
} // namespace ith
