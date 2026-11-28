// ith.play.keep_simulation_changes (ADR-0053 decisions 24 to 27, docs/specs/play-in-editor.md 3.12):
// copies the component values of game-world entities back to the same entities of the editor world,
// matched by stable id, as one editor transaction named "Keep Simulation Changes". It is an Action
// that records its own history entry: the registry refuses it while a level opens
// ("editor.document.busy") and while an interaction holds a transaction open in either world
// ("editor.command.interaction_open"), and it refuses itself without a session
// ("editor.play.not_running") and when called from inside another command that holds an editor-world
// transaction open ("editor.command.interaction_open").
//
// Per entity, each component type it has in both worlds that is neither Transient nor EditorOnly,
// except StableId (identity) and ChildOf (structure), is compared through TypeInfo::equals, which
// compares reflected fields only. A type that may hold entities is copied first and its references
// mapped from the game world to the editor world (remapEntities, ADR-0032): an entity with a stable
// id becomes the editor entity with that id; one without becomes the editor entity in the same slot
// with the same generation, when that one is alive and has no stable id either (copyWorld keeps
// slots and generations, and the editor world doesn't change during play, so only an entity copied
// from the editor world matches); anything else becomes null, with an
// "editor.play.reference_dropped" warning when the value is kept. Every Entity field must be
// reflected for this (the precondition of ith/reflection/Entities.h). Other types are compared in
// place and copied only when they differ.
//
// A differing value is kept as a `set` of the whole value, or an Add that replaces it for an
// immutable type (ADR-0030). Within an entity the Adds come first: the hooks an Add runs in the
// editor world (onReplace, onInsert) can then not overwrite a value the call keeps. Those hooks run
// in the editor world, which has none of the game's resources (no Time), and the commands they
// record are applied to the editor world too. Components only in one world, a changed parent and a
// changed enabled state (Disabled) are structural: reported in `ignored`, never applied. When the
// parent changed, the LocalTransform is reported as "parent changed" too and not kept: it is
// relative to the game parent. The game world doesn't change. The cost is linear in the entities
// given times their components.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/reflection/Entities.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {

namespace {

using detail::idValue;
using detail::invalidArgs;
using serialization::Value;

constexpr std::string_view kCommand = kKeepSimulationChanges;

// The reasons of `skipped` and `ignored` (docs/specs/play-in-editor.md 3.12).
constexpr std::string_view kNotFound = "editor.play.not_found";
constexpr std::string_view kNotInLevel = "not in the level";
constexpr std::string_view kAddedDuringPlay = "added during play";
constexpr std::string_view kRemovedDuringPlay = "removed during play";
constexpr std::string_view kParentChanged = "parent changed";
constexpr std::string_view kEnabledChanged = "enabled state changed";

// Maps game-world handles to the editor world (see the file comment); a handle that maps to nothing
// becomes null and counts as dropped.
struct ToEditor {
    const World* game = nullptr;
    const World* editor = nullptr;
    std::size_t dropped = 0;

    // The editor entity `entity` of the game world stands for; null when none.
    [[nodiscard]] Entity twinOf(Entity entity) const {
        if (entity.isNull()) {
            return entity;
        }
        if (const auto* id = game->get<const StableId>(entity)) {
            return editor->findByStableId(id->id);
        }
        const Entity same = Entity::fromParts(entity.index(), entity.generation(), editor->id());
        return editor->isAlive(same) && editor->get<const StableId>(same) == nullptr ? same : Entity();
    }

    static Entity map(void* context, Entity entity) {
        auto& self = *static_cast<ToEditor*>(context);
        const Entity twin = self.twinOf(entity);
        if (twin.isNull() && !entity.isNull()) {
            ++self.dropped;
        }
        return twin;
    }
};

// Whether a component type takes part at all: neither Transient nor EditorOnly, and not StableId.
bool considered(const TypeInfo& type) {
    return !hasFlags(type.flags, ReflectionFlags::Transient) && !hasFlags(type.flags, ReflectionFlags::EditorOnly) &&
           type.id != typeOf<StableId>().id;
}

// The reason a component only one world has is ignored: `gameOnly` when it was added during play.
std::string_view structuralReason(const TypeInfo& type, bool gameOnly) {
    if (type.id == typeOf<ChildOf>().id) {
        return kParentChanged;
    }
    if (type.id == typeOf<Disabled>().id) {
        return kEnabledChanged;
    }
    return gameOnly ? kAddedDuringPlay : kRemovedDuringPlay;
}

// Whether the game entity's parent link differs from the editor entity's: present in one world only,
// another parent (mapped as references are), or another despawnWithParent.
bool parentChanged(const ToEditor& toEditor, Entity gameEntity, Entity editorEntity) {
    const auto* gameLink = toEditor.game->get<const ChildOf>(gameEntity);
    const auto* editorLink = toEditor.editor->get<const ChildOf>(editorEntity);
    if (gameLink == nullptr || editorLink == nullptr) {
        return gameLink != editorLink;
    }
    return toEditor.twinOf(gameLink->parent) != editorLink->parent ||
           gameLink->despawnWithParent != editorLink->despawnWithParent;
}

Value reasonEntry(const Uuid& id, std::string_view reason) {
    Value entry = Value::object();
    entry.set("id", idValue(id));
    entry.set("reason", Value::string(std::string(reason)));
    return entry;
}

Value ignoredEntry(const Uuid& id, const TypeInfo& type, std::string_view reason) {
    Value entry = reasonEntry(id, reason);
    entry.set("component", Value::string(std::string(type.name)));
    return entry;
}

// The ids of "ids", or the play selection when it is missing or null; repeats dropped.
Expected<std::vector<Uuid>> idsOf(const Value& args, const Selection& selection) {
    const Value* list = args.kind() == Value::Kind::Object ? args.find("ids") : nullptr;
    if (list == nullptr || list->kind() == Value::Kind::Null) {
        return selection.entities();
    }
    if (list->kind() != Value::Kind::Array) {
        return invalidArgs(kCommand, "\"ids\" must be an array of stable ids");
    }
    std::vector<Uuid> ids;
    std::unordered_set<Uuid> seen;
    for (const Value& element : list->elements()) {
        if (element.kind() != Value::Kind::String) {
            return invalidArgs(kCommand, "\"ids\" must be an array of stable ids");
        }
        Expected<Uuid> id = detail::parseId(kCommand, element.asString());
        if (!id) {
            return makeUnexpected(std::move(id).error());
        }
        if (seen.insert(*id).second) {
            ids.push_back(*id);
        }
    }
    return ids;
}

Expected<void> canKeep(EditorSession& session) {
    if (session.play() == nullptr) {
        return makeError("editor.play.not_running", "no play session is running");
    }
    // Nested calls: the registry checks interactions only for a command run on its own, so a command
    // that holds an editor-world transaction open and runs this one would get it joined into its own.
    if (session.document().transactions().isOpen()) {
        return makeError("editor.command.interaction_open",
                         std::string(kCommand) + ": an editor transaction is open; finish or cancel it first");
    }
    return {};
}

// One call's comparison: what it keeps, skips, ignores and warns about, and the patches it applies.
class Collector {
public:
    Collector(const World& game, const World& editor) noexcept
        : m_game(&game), m_editor(&editor), m_toEditor{.game = &game, .editor = &editor} {}

    // Compares the entity with stable id `id` in both worlds and collects its changes.
    void entity(const Uuid& id) {
        const Entity gameEntity = m_game->findByStableId(id);
        if (gameEntity.isNull()) {
            m_skipped.elements().push_back(reasonEntry(id, kNotFound));
            return;
        }
        const Entity editorEntity = m_editor->findByStableId(id);
        if (editorEntity.isNull()) {
            m_skipped.elements().push_back(reasonEntry(id, kNotInLevel));
            return;
        }
        m_id = &id;
        m_editorEntity = editorEntity;
        m_reparented = parentChanged(m_toEditor, gameEntity, editorEntity);
        // The Adds of immutable types go in `m_patch` at once, the sets after them (see the file comment).
        m_patch = EntityPatch{.entity = editorEntity};
        m_sets.clear();
        m_components = Value::array();
        m_game->forEachComponent(gameEntity,
                                 [&](const TypeInfo& type, const void* gameValue) { component(type, gameValue); });
        m_editor->forEachComponent(editorEntity, [&](const TypeInfo& type, const void* /*value*/) {
            if (considered(type) && !m_game->has(gameEntity, type)) {
                m_ignored.elements().push_back(ignoredEntry(id, type, structuralReason(type, false)));
            }
        });
        for (ComponentChange& set : m_sets) {
            m_patch.changes.push_back(std::move(set));
        }
        if (!m_patch.changes.empty()) {
            Value entry = Value::object();
            entry.set("id", idValue(id));
            entry.set("components", std::move(m_components));
            m_kept.elements().push_back(std::move(entry));
            m_patches.push_back(std::move(m_patch));
        }
    }

    [[nodiscard]] const std::vector<EntityPatch>& patches() const noexcept { return m_patches; }

    // The command's result.
    [[nodiscard]] Value result() {
        Value result = Value::object();
        result.set("kept", std::move(m_kept));
        result.set("skipped", std::move(m_skipped));
        result.set("ignored", std::move(m_ignored));
        result.set("warnings", std::move(m_warnings));
        return result;
    }

private:
    // One component of the current entity's game twin.
    void component(const TypeInfo& type, const void* gameValue) {
        if (!considered(type)) {
            return;
        }
        const void* editorValue = m_editor->get(m_editorEntity, type);
        if (editorValue == nullptr) {
            m_ignored.elements().push_back(ignoredEntry(*m_id, type, structuralReason(type, true)));
            return;
        }
        if (type.id == typeOf<ChildOf>().id) {
            if (m_reparented) {
                m_ignored.elements().push_back(ignoredEntry(*m_id, type, kParentChanged));
            }
            return;
        }
        const std::size_t droppedBefore = m_toEditor.dropped;
        OwnedValue value = differing(type, gameValue, editorValue);
        if (!value.hasValue()) {
            return;
        }
        if (m_reparented && type.id == typeOf<LocalTransform>().id) {
            m_ignored.elements().push_back(ignoredEntry(*m_id, type, kParentChanged));
            return;
        }
        if (m_toEditor.dropped != droppedBefore) {
            warnDropped(type);
        }
        m_components.elements().push_back(Value::string(std::string(type.name)));
        if (hasFlags(type.flags, ReflectionFlags::Immutable)) {
            m_patch.changes.push_back(ComponentChange::makeAdd(std::move(value)));
        } else {
            Patch whole(type);
            whole.set(PropertyPath(type), std::move(value));
            m_sets.push_back(ComponentChange::makePatch(std::move(whole)));
        }
    }

    // The game value as the editor world would hold it, when it differs from `editorValue`; empty
    // when they are equal. Only a type that may hold entities is copied before the comparison.
    OwnedValue differing(const TypeInfo& type, const void* gameValue, const void* editorValue) {
        if (!type.containsEntities) {
            return type.equals(gameValue, editorValue) ? OwnedValue() : OwnedValue::copyOf(type, gameValue);
        }
        OwnedValue value = OwnedValue::copyOf(type, gameValue);
        remapEntities(type, value.get(), {.function = &ToEditor::map, .context = &m_toEditor});
        return type.equals(value.get(), editorValue) ? OwnedValue() : std::move(value);
    }

    void warnDropped(const TypeInfo& type) {
        Value warning = Value::object();
        warning.set("code", Value::string("editor.play.reference_dropped"));
        warning.set("message", Value::string(m_id->toString() + " " + std::string(type.name) +
                                             ": a reference to an entity that is not in the level becomes null"));
        m_warnings.elements().push_back(std::move(warning));
    }

    const World* m_game;
    const World* m_editor;
    ToEditor m_toEditor;
    Value m_kept = Value::array();
    Value m_skipped = Value::array();
    Value m_ignored = Value::array();
    Value m_warnings = Value::array();
    std::vector<EntityPatch> m_patches;
    // The current entity.
    const Uuid* m_id = nullptr;
    Entity m_editorEntity;
    bool m_reparented = false;
    EntityPatch m_patch;
    std::vector<ComponentChange> m_sets;
    Value m_components = Value::array();
};

Expected<Value> keep(EditorSession& session, const Value& args) {
    const PlaySession& play = *session.play();
    EditorDocument& document = session.document();
    Expected<std::vector<Uuid>> ids = idsOf(args, play.selection());
    if (!ids) {
        return makeUnexpected(std::move(ids).error());
    }
    Collector collector(play.world(), document.world());
    for (const Uuid& id : *ids) {
        collector.entity(id);
    }
    if (!collector.patches().empty()) {
        // Recorded at once, though a session runs: ADR-0037 rule 10's one exception. The selection
        // doesn't change, so the entry carries the editor selection before and after.
        TransactionScope scope = document.transactions().begin("Keep Simulation Changes");
        if (Expected<void> applied = scope.apply(collector.patches()); !applied) {
            return makeUnexpected(std::move(applied).error()); // the scope cancels: nothing is kept
        }
        scope.commit();
    }
    return collector.result();
}

} // namespace

void registerKeepSimulationChanges(CommandRegistry& registry) {
    registry.add({.id = std::string(kCommand),
                  .displayName = "Keep Simulation Changes",
                  .tooltip = "Copy the selected entities' values from the game back to the level",
                  .category = "Play",
                  .kind = CommandKind::Action,
                  .canExecute = &canKeep,
                  .execute = &keep});
}

} // namespace ith
