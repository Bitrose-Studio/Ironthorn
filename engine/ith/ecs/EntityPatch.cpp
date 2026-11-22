#include "ith/ecs/EntityPatch.h"

#include "ith/ecs/Hooks.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"
#include "ith/foundation/Uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith::detail {

namespace {

// True for the components the World keeps itself: Children and DisabledInHierarchy.
bool isWorldOwned(const TypeInfo& type) {
    return type.id == typeOf<Children>().id || type.id == typeOf<DisabledInHierarchy>().id;
}

// True when values of the registered `index` run hooks when they are added or replaced.
bool hasAddHooks(ComponentIndex index) {
    const ComponentHooks* hooks = useComponentType(index);
    return hooks != nullptr && (hooks->onAdd != nullptr || hooks->onInsert != nullptr || hooks->onReplace != nullptr);
}

// True when values of the registered `index` run any hook.
bool hasHooks(ComponentIndex index) {
    const ComponentHooks* hooks = useComponentType(index);
    return hooks != nullptr && (hasAddHooks(index) || hooks->onRemove != nullptr);
}

// An entity map through a table of handle bits; handles not in it stay as they are.
using HandleTable = std::unordered_map<std::uint64_t, Entity>;

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMap::function has
Entity mapThrough(void* context, Entity entity) {
    const auto& table = *static_cast<const HandleTable*>(context);
    const auto found = table.find(entity.bits());
    return found != table.end() ? found->second : entity;
}

// Drops the changes of one entity's inverse that a later Add of the same type makes pointless: an
// Add or a Patch of a type without add hooks, whose replacement has no effect the later Add doesn't
// have. Keeps an inverse from growing each time it is undone and redone, as the restores of hook
// side effects would otherwise make it.
void compact(std::vector<ComponentChange>& changes) {
    std::vector<ComponentChange> kept;
    kept.reserve(changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const ComponentChange& change = changes[i];
        bool redundant = false;
        if (change.kind != ComponentChange::Kind::Remove && change.type != nullptr &&
            !hasAddHooks(registerComponentType(*change.type))) {
            redundant = std::any_of(changes.begin() + static_cast<std::ptrdiff_t>(i) + 1, changes.end(),
                                    [&](const ComponentChange& later) {
                                        return later.kind == ComponentChange::Kind::Add && later.type != nullptr &&
                                               later.type->id == change.type->id;
                                    });
        }
        if (!redundant) {
            kept.push_back(std::move(changes[i]));
        }
    }
    changes = std::move(kept);
}

} // namespace

// World::applyPatches in three passes. planPatch() checks every change against the state the
// changes before it leave, kept in `slots` and the spawned and despawned sets without touching the
// World, and records the steps. commit() makes them, capturing what only the World knows at that
// moment: where an entity stood among its siblings, the components of a despawned entity, and the
// components that hooks changed. inverse() then builds the patches that undo the call.
struct PatchApplier {
    // A component that an earlier change touched: whether it is there and its value.
    struct Slot {
        Entity entity;
        ComponentIndex type = kNoComponent;
        bool present = false;
        OwnedValue value;
    };

    enum class Step : std::uint8_t {
        Write,   // write the value in place
        Add,     // add or replace through World::add
        Remove,  // remove through World::remove
        Spawn,   // make the reserved handle alive, without components
        Despawn, // despawn the entity
        Place,   // place the entity before `sibling`
    };

    struct Planned {
        Entity entity;
        ComponentIndex type = kNoComponent;
        Step step = Step::Write;
        OwnedValue value;
        // The entity patch the step belongs to.
        std::size_t patch = 0;
        // Write, Add and Remove of a Modify: the change that undoes the step, and the restores of what
        // its hooks changed on the entity, which commit() captures.
        ComponentChange undo;
        std::vector<ComponentChange> restores;
        // Place: the sibling.
        Entity sibling;
    };

    // A slot's key: the entity's bits and the component index.
    struct SlotKey {
        std::uint64_t entity = 0;
        ComponentIndex type = kNoComponent;

        friend bool operator==(const SlotKey&, const SlotKey&) = default;
    };

    struct SlotKeyHash {
        std::size_t operator()(const SlotKey& key) const noexcept {
            return static_cast<std::size_t>(hashCombine(key.entity, key.type));
        }
    };

    using Components = std::vector<std::pair<ComponentIndex, OwnedValue>>;

    World& world;
    // The slots, found through `slotIndex`, so planning M changes takes O(M) lookups, not O(M^2).
    std::vector<Slot> slots;
    std::unordered_map<SlotKey, std::size_t, SlotKeyHash> slotIndex;
    // The entity of each StableId that a slot holds (present), so ownerOf needn't scan the slots.
    std::unordered_map<Uuid, Entity> slotStableIds;
    std::vector<Planned> planned;
    // The handles reserved for Spawns, and the entities spawned and despawned by the plan, by bits.
    std::vector<Entity> reserved;
    std::unordered_set<std::uint64_t> spawned;
    std::unordered_set<std::uint64_t> despawned;

    // Filled by commit(), per entity patch: where a Modify's entity stood before it first moved,
    // and the Spawn that undoes a Despawn. Then the despawned entities in commit order, which the
    // inverse names by placeholders.
    std::vector<std::optional<Entity>> inversePlace;
    std::vector<EntityPatch> despawnInverse;
    std::vector<Entity> despawnedOrder;

    // True when the entity is alive as the earlier changes leave the world.
    [[nodiscard]] bool alive(Entity entity) const {
        if (entity.isNull() || entity.worldId() != world.id() || despawned.contains(entity.bits())) {
            return false;
        }
        return spawned.contains(entity.bits()) || world.isAlive(entity);
    }

    [[nodiscard]] const Slot* findSlot(Entity entity, ComponentIndex type) const {
        const auto found = slotIndex.find(SlotKey{.entity = entity.bits(), .type = type});
        return found != slotIndex.end() ? &slots[found->second] : nullptr;
    }

    // The entity's value of `type` as the earlier changes leave it; null when it has none.
    [[nodiscard]] const void* current(Entity entity, ComponentIndex type) const {
        if (const Slot* slot = findSlot(entity, type)) {
            return slot->present ? slot->value.get() : nullptr;
        }
        return world.findComponent(entity, type);
    }

    void record(Entity entity, ComponentIndex type, bool present, const OwnedValue& value) {
        const auto [found, inserted] =
            slotIndex.try_emplace(SlotKey{.entity = entity.bits(), .type = type}, slots.size());
        if (inserted) {
            slots.push_back(Slot{.entity = entity, .type = type});
        }
        Slot& slot = slots[found->second];
        if (type == componentIndexOf<StableId>()) {
            if (slot.present) {
                const Uuid& old = static_cast<const StableId*>(slot.value.get())->id;
                if (const auto owner = slotStableIds.find(old);
                    owner != slotStableIds.end() && owner->second == entity) {
                    slotStableIds.erase(owner);
                }
            }
            if (present) {
                slotStableIds[static_cast<const StableId*>(value.get())->id] = entity;
            }
        }
        slot.present = present;
        slot.value = value;
    }

    [[nodiscard]] Entity parentOf(Entity entity) const {
        const auto* link = static_cast<const ChildOf*>(current(entity, componentIndexOf<ChildOf>()));
        return link != nullptr ? link->parent : Entity();
    }

    // True when a live entity has the entity as its parent, as the earlier changes leave the world.
    [[nodiscard]] bool hasChildren(Entity entity) const {
        if (const auto* children =
                static_cast<const Children*>(world.findComponent(entity, componentIndexOf<Children>()))) {
            for (const Entity child : children->entities) {
                if (alive(child) && parentOf(child) == entity) {
                    return true;
                }
            }
        }
        const ComponentIndex childOf = componentIndexOf<ChildOf>();
        return std::ranges::any_of(slots, [&](const Slot& slot) {
            return slot.type == childOf && slot.present && alive(slot.entity) &&
                   static_cast<const ChildOf*>(slot.value.get())->parent == entity;
        });
    }

    // The entity whose StableId is `id` as the earlier changes leave the world; null when none.
    [[nodiscard]] Entity ownerOf(const Uuid& id) const {
        if (const auto found = slotStableIds.find(id); found != slotStableIds.end()) {
            return found->second;
        }
        const ComponentIndex stableId = componentIndexOf<StableId>();
        const Entity owner = world.findByStableId(id);
        // An owner whose StableId an earlier change replaced or removed no longer has this id.
        return owner.isNull() || findSlot(owner, stableId) != nullptr ? Entity() : owner;
    }

    // Why the new `value` of `type` can't be stored on `entity`; empty when it can. As
    // World::acceptsValue, against the state the earlier changes leave, without asserting.
    [[nodiscard]] std::string refusal(Entity entity, ComponentIndex type, const void* value) const {
        if (type == componentIndexOf<StableId>()) {
            const Uuid& id = static_cast<const StableId*>(value)->id;
            if (id.isNil()) {
                return "a StableId can't be the nil id";
            }
            const Entity owner = ownerOf(id);
            return owner.isNull() || owner == entity
                       ? std::string()
                       : "another entity of the world already has StableId " + id.toString();
        }
        if (type != componentIndexOf<ChildOf>()) {
            return {};
        }
        const Entity parent = static_cast<const ChildOf*>(value)->parent;
        if (!alive(parent)) {
            return "a ChildOf's parent must be a live entity of the same world";
        }
        // Bounded, in case earlier changes left a cycle above the parent that doesn't reach entity.
        std::size_t steps = 0;
        const std::size_t limit = world.entityCount() + spawned.size();
        for (Entity ancestor = parent; !ancestor.isNull() && steps <= limit; ++steps) {
            if (ancestor == entity) {
                return "a ChildOf's parent can't be the entity itself or one of its descendants";
            }
            ancestor = parentOf(ancestor);
        }
        return {};
    }

    // Checks one component change and plans it. Only a Modify keeps the change that undoes it; a
    // Spawn is undone by a Despawn.
    Expected<void> plan(Entity entity, const ComponentChange& change, const std::string& where, std::size_t patch,
                        bool modify) {
        const TypeInfo* type = change.type;
        if (type == nullptr || type->role != TypeRole::Component) {
            return makeError("ecs.patch.not_a_component",
                             where + (type == nullptr ? std::string("the change has no type")
                                                      : std::string(type->name) + " is not a component type"));
        }
        if (isWorldOwned(*type)) {
            return makeError("ecs.patch.world_owned", where + std::string(type->name) + " is kept by the World");
        }
        const ComponentIndex index = registerComponentType(*type);
        const void* before = current(entity, index);

        OwnedValue after;
        Step step = Step::Add;
        ComponentChange undo;
        switch (change.kind) {
            case ComponentChange::Kind::Patch: {
                if (before == nullptr) {
                    return makeError("ecs.patch.missing_component",
                                     where + "the entity has no " + std::string(type->name) + " to patch");
                }
                if (change.patch.rootType() == nullptr || change.patch.rootType()->id != type->id) {
                    return makeError("reflection.patch.wrong_type",
                                     where + "the patch is not a patch of " + std::string(type->name));
                }
                Expected<PreparedPatch> prepared = preparePatch(change.patch, before);
                if (!prepared) {
                    Error error = std::move(prepared).error();
                    return makeError(std::move(error.code), where + error.message);
                }
                after = std::move(prepared->value);
                step = hasFlags(type->flags, ReflectionFlags::Immutable) ? Step::Add : Step::Write;
                undo = ComponentChange::makePatch(std::move(prepared->inverse));
                break;
            }
            case ComponentChange::Kind::Add:
                if (!change.value.hasValue() || change.value.type()->id != type->id) {
                    return makeError("reflection.patch.wrong_type",
                                     where + "the value is not a " + std::string(type->name));
                }
                after = change.value;
                undo = before != nullptr ? ComponentChange::makeAdd(OwnedValue::copyOf(*type, before))
                                         : ComponentChange::makeRemove(*type);
                break;
            case ComponentChange::Kind::Remove:
                if (before == nullptr) {
                    return makeError("ecs.patch.missing_component",
                                     where + "the entity has no " + std::string(type->name) + " to remove");
                }
                step = Step::Remove;
                undo = ComponentChange::makeAdd(OwnedValue::copyOf(*type, before));
                break;
        }

        if (step != Step::Remove) {
            if (const std::string reason = refusal(entity, index, after.get()); !reason.empty()) {
                return makeError("ecs.patch.refused_value", where + reason);
            }
        }
        record(entity, index, step != Step::Remove, after);
        Planned& planning = planned.emplace_back(
            Planned{.entity = entity, .type = index, .step = step, .value = std::move(after), .patch = patch});
        if (modify) {
            planning.undo = std::move(undo);
        }
        return {};
    }

    // Checks and plans placing an entity before `sibling`.
    Expected<void> planPlace(Entity entity, Entity sibling, const std::string& where, std::size_t patch) {
        if (!sibling.isNull()) {
            if (!alive(sibling)) {
                return makeError("ecs.patch.stale_entity", where + "the sibling to place before is not alive");
            }
            if (sibling == entity || parentOf(sibling) != parentOf(entity)) {
                return makeError("ecs.patch.invalid_place",
                                 where + "the sibling to place before must be another entity under the same parent");
            }
        }
        planned.push_back(Planned{.entity = entity, .step = Step::Place, .patch = patch, .sibling = sibling});
        return {};
    }

    // Checks and plans one entity patch.
    Expected<void> planPatch(const EntityPatch& patch, std::size_t patchIndex) {
        const std::string prefix = "entity patch " + std::to_string(patchIndex) + ": ";
        const auto at = [&](std::size_t change) {
            return "entity patch " + std::to_string(patchIndex) + ", change " + std::to_string(change) + ": ";
        };
        switch (patch.op) {
            case EntityOp::Modify:
                if (!alive(patch.entity)) {
                    return makeError("ecs.patch.stale_entity", prefix + "the entity is not alive in this world");
                }
                for (std::size_t change = 0; change < patch.changes.size(); ++change) {
                    if (Expected<void> result = plan(patch.entity, patch.changes[change], at(change), patchIndex, true);
                        !result) {
                        return result;
                    }
                }
                break;
            case EntityOp::Spawn: {
                // applyPatches replaced the placeholder by a reserved handle.
                spawned.insert(patch.entity.bits());
                planned.push_back(Planned{.entity = patch.entity, .step = Step::Spawn, .patch = patchIndex});
                // ChildOf first, so the others are added under the parent (ADR-0034 rule 12).
                for (const bool parentPass : {true, false}) {
                    for (std::size_t change = 0; change < patch.changes.size(); ++change) {
                        const ComponentChange& component = patch.changes[change];
                        if (component.kind != ComponentChange::Kind::Add) {
                            return makeError("ecs.patch.invalid_spawn", at(change) + "a Spawn takes Adds only");
                        }
                        const bool isParent = component.type != nullptr && component.type->id == typeOf<ChildOf>().id;
                        if (isParent != parentPass) {
                            continue;
                        }
                        if (Expected<void> result = plan(patch.entity, component, at(change), patchIndex, false);
                            !result) {
                            return result;
                        }
                    }
                }
                break;
            }
            case EntityOp::Despawn: {
                if (!patch.changes.empty() || patch.placeBefore.has_value()) {
                    return makeError("ecs.patch.invalid_despawn", prefix + "a Despawn has no changes and no placement");
                }
                if (!alive(patch.entity)) {
                    return makeError("ecs.patch.stale_entity", prefix + "the entity is not alive in this world");
                }
                if (hasChildren(patch.entity)) {
                    return makeError("ecs.patch.has_children",
                                     prefix + "the entity has children; detach or despawn them first");
                }
                const ComponentIndex stableId = componentIndexOf<StableId>();
                if (current(patch.entity, stableId) != nullptr) {
                    record(patch.entity, stableId, false, OwnedValue());
                }
                despawned.insert(patch.entity.bits());
                planned.push_back(Planned{.entity = patch.entity, .step = Step::Despawn, .patch = patchIndex});
                break;
            }
        }
        if (patch.placeBefore.has_value()) {
            return planPlace(patch.entity, *patch.placeBefore, prefix, patchIndex);
        }
        return {};
    }

    // Copies of the entity's components, except `skip`, the World's own and, unless `transient`,
    // Transient ones.
    [[nodiscard]] Components componentsOf(Entity entity, ComponentIndex skip, bool transient) const {
        Components result;
        std::as_const(world).forEachComponent(entity, [&](const TypeInfo& type, const void* value) {
            const ComponentIndex index = registerComponentType(type);
            if (index == skip || isWorldOwned(type) ||
                (!transient && hasFlags(type.flags, ReflectionFlags::Transient))) {
                return;
            }
            result.emplace_back(index, OwnedValue::copyOf(type, value));
        });
        return result;
    }

    // The changes that put back what a step's hooks changed on the entity: an Add of each
    // component whose value differs from `before` or that went, a Remove of each that appeared.
    [[nodiscard]] std::vector<ComponentChange> restoresOf(Entity entity, ComponentIndex skip,
                                                          const Components& before) const {
        std::vector<ComponentChange> restores;
        for (const auto& [index, value] : before) {
            const void* now = world.findComponent(entity, index);
            if (now == nullptr || !componentType(index).equals(now, value.get())) {
                restores.push_back(ComponentChange::makeAdd(value));
            }
        }
        for (const auto& entry : componentsOf(entity, skip, false)) {
            const ComponentIndex index = entry.first;
            if (std::ranges::none_of(before, [index](const auto& old) { return old.first == index; })) {
                restores.push_back(ComponentChange::makeRemove(componentType(index)));
            }
        }
        return restores;
    }

    // Records where a Modify's entity stands, the first time it is about to move.
    void notePlace(const Planned& change) {
        std::optional<Entity>& place = inversePlace[change.patch];
        if (!place.has_value()) {
            place = world.nextSiblingOf(change.entity);
        }
    }

    // Makes the planned steps in order.
    void commit(std::span<const EntityPatch> patches) {
        inversePlace.assign(patches.size(), std::nullopt);
        despawnInverse.resize(patches.size());
        const ComponentIndex childOf = componentIndexOf<ChildOf>();
        for (Planned& change : planned) {
            const bool modify = patches[change.patch].op == EntityOp::Modify;
            switch (change.step) {
                case Step::Write: {
                    void* target = world.writeComponent(change.entity, change.type);
                    ITH_ASSERT_MSG(target != nullptr, "ecs: applyPatches: a hook removed a component that a later "
                                                      "change patches");
                    if (target != nullptr) {
                        change.value.moveInto(target);
                    }
                    break;
                }
                case Step::Add:
                case Step::Remove: {
                    if (modify && change.type == childOf) {
                        notePlace(change);
                    }
                    const bool capture = modify && hasHooks(change.type);
                    Components before;
                    if (capture) {
                        before = componentsOf(change.entity, change.type, false);
                    }
                    if (change.step == Step::Add) {
                        [[maybe_unused]] const void* added =
                            world.add(change.entity, componentType(change.type), change.value.get());
                        ITH_ASSERT_MSG(added != nullptr, "ecs: applyPatches: a checked value was refused");
                    } else {
                        [[maybe_unused]] const bool removed = world.removeComponent(change.entity, change.type);
                        ITH_ASSERT_MSG(removed, "ecs: applyPatches: a hook removed a component that a later change "
                                                "removes");
                    }
                    if (capture) {
                        change.restores = restoresOf(change.entity, change.type, before);
                    }
                    break;
                }
                case Step::Spawn:
                    static_cast<void>(world.placeReserved(change.entity, world.findOrCreateArchetype({})));
                    break;
                case Step::Despawn: {
                    EntityPatch& undo = despawnInverse[change.patch];
                    undo = EntityPatch{.op = EntityOp::Spawn,
                                       .entity = change.entity,
                                       .placeBefore = world.nextSiblingOf(change.entity)};
                    for (auto& entry : componentsOf(change.entity, kNoComponent, true)) {
                        undo.changes.push_back(ComponentChange::makeAdd(std::move(entry.second)));
                    }
                    [[maybe_unused]] const bool gone = world.despawn(change.entity);
                    ITH_ASSERT_MSG(gone, "ecs: applyPatches: a checked despawn failed");
                    despawnedOrder.push_back(change.entity);
                    break;
                }
                case Step::Place: {
                    if (modify) {
                        notePlace(change);
                    }
                    [[maybe_unused]] const bool placed = world.placeBefore(change.entity, change.sibling);
                    ITH_ASSERT_MSG(placed, "ecs: applyPatches: a checked placement failed");
                    break;
                }
            }
        }
    }

    // The patches that undo the committed ones: the entity patches in reverse order, each Modify
    // with its changes in reverse order and every change followed by the restores of its hooks'
    // side effects. Entities the call despawned are named by placeholders.
    [[nodiscard]] std::vector<EntityPatch> inverse(std::span<const EntityPatch> patches) {
        std::vector<EntityPatch> result;
        result.reserve(patches.size());
        std::size_t end = planned.size();
        for (std::size_t patch = patches.size(); patch-- > 0;) {
            std::size_t begin = end;
            while (begin > 0 && planned[begin - 1].patch == patch) {
                --begin;
            }
            switch (patches[patch].op) {
                case EntityOp::Modify: {
                    EntityPatch& undo = result.emplace_back(
                        EntityPatch{.entity = patches[patch].entity, .placeBefore = inversePlace[patch]});
                    for (std::size_t step = end; step-- > begin;) {
                        Planned& change = planned[step];
                        if (change.step == Step::Place) {
                            continue;
                        }
                        undo.changes.push_back(std::move(change.undo));
                        for (ComponentChange& restore : change.restores) {
                            undo.changes.push_back(std::move(restore));
                        }
                    }
                    compact(undo.changes);
                    break;
                }
                case EntityOp::Spawn:
                    result.push_back(EntityPatch{.op = EntityOp::Despawn, .entity = patches[patch].entity});
                    break;
                case EntityOp::Despawn:
                    result.push_back(std::move(despawnInverse[patch]));
                    break;
            }
            end = begin;
        }
        if (!despawnedOrder.empty()) {
            HandleTable placeholders;
            for (std::size_t i = 0; i < despawnedOrder.size(); ++i) {
                placeholders.emplace(despawnedOrder[i].bits(), Entity::fromParts(static_cast<std::uint32_t>(i), 1, 0));
            }
            const EntityMap map{.function = &mapThrough, .context = &placeholders};
            for (EntityPatch& undo : result) {
                remapEntities(undo, map);
            }
        }
        return result;
    }

    // Gives back the handles reserved for Spawns of a call that failed; they stay stale.
    void releaseReserved() {
        for (const Entity entity : reserved) {
            world.freeSlot(entity.index());
        }
        reserved.clear();
    }
};

} // namespace ith::detail

namespace ith {

void remapEntities(EntityPatch& patch, const EntityMap& map) {
    ITH_ASSERT(map.function != nullptr);
    if (map.function == nullptr) {
        return;
    }
    patch.entity = map.function(map.context, patch.entity);
    if (patch.placeBefore.has_value()) {
        patch.placeBefore = map.function(map.context, *patch.placeBefore);
    }
    for (ComponentChange& change : patch.changes) {
        switch (change.kind) {
            case ComponentChange::Kind::Patch:
                remapEntities(change.patch, map);
                break;
            case ComponentChange::Kind::Add:
                if (change.value.hasValue()) {
                    remapEntities(*change.value.type(), change.value.get(), map);
                }
                break;
            case ComponentChange::Kind::Remove:
                break;
        }
    }
}

Expected<std::vector<EntityPatch>> World::applyPatches(std::span<const EntityPatch> patches) {
    if (!canChangeStructure()) {
        return makeError("ecs.patch.iterating", "applyPatches while a query iterates or a system runs");
    }
    detail::PatchApplier applier{.world = *this};

    // Each Spawn's placeholder becomes a reserved handle, throughout a copy of the patches.
    std::vector<EntityPatch> remapped;
    if (std::ranges::any_of(patches, [](const EntityPatch& patch) { return patch.op == EntityOp::Spawn; })) {
        detail::HandleTable handles;
        for (std::size_t index = 0; index < patches.size(); ++index) {
            const EntityPatch& patch = patches[index];
            if (patch.op != EntityOp::Spawn) {
                continue;
            }
            const bool placeholder = !patch.entity.isNull() && patch.entity.worldId() == 0;
            if (!placeholder || handles.contains(patch.entity.bits())) {
                applier.releaseReserved();
                return makeError("ecs.patch.invalid_spawn",
                                 "entity patch " + std::to_string(index) +
                                     ": a Spawn's entity must be a placeholder (world id 0, not null) that no other "
                                     "Spawn of the call uses");
            }
            const Entity handle = reserveEntity();
            applier.reserved.push_back(handle);
            handles.emplace(patch.entity.bits(), handle);
        }
        remapped.assign(patches.begin(), patches.end());
        const EntityMap map{.function = &detail::mapThrough, .context = &handles};
        for (EntityPatch& patch : remapped) {
            remapEntities(patch, map);
        }
        patches = remapped;
    }

    for (std::size_t patchIndex = 0; patchIndex < patches.size(); ++patchIndex) {
        if (Expected<void> result = applier.planPatch(patches[patchIndex], patchIndex); !result) {
            applier.releaseReserved();
            return makeUnexpected(std::move(result).error());
        }
    }
    applier.commit(patches);
    return applier.inverse(patches);
}

} // namespace ith
