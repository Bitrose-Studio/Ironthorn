#include "ith/ecs/EntityPatch.h"

#include "ith/ecs/World.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"
#include "ith/foundation/Uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ith::detail {

// World::applyPatches in two passes. plan() checks every change against the state the changes
// before it leave, kept in `slots` without touching the World, and records the value to store and
// the undoing change. commit() then makes the planned changes.
struct PatchApplier {
    // A component that an earlier change touched: whether it is there and its value.
    struct Slot {
        Entity entity;
        ComponentIndex type = kNoComponent;
        bool present = false;
        OwnedValue value;
    };

    enum class Step : std::uint8_t {
        Write,  // write the value in place
        Add,    // add or replace through World::add
        Remove, // remove through World::remove
    };

    struct Planned {
        Entity entity;
        ComponentIndex type = kNoComponent;
        Step step = Step::Write;
        OwnedValue value;
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

    World& world;
    // The slots, found through `slotIndex`, so planning M changes takes O(M) lookups, not O(M^2).
    std::vector<Slot> slots;
    std::unordered_map<SlotKey, std::size_t, SlotKeyHash> slotIndex;
    // The entity of each StableId that a slot holds (present), so ownerOf needn't scan the slots.
    std::unordered_map<Uuid, Entity> slotStableIds;
    std::vector<Planned> planned;

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
        if (parent.isNull() || parent.worldId() != world.id() || !world.isAlive(parent)) {
            return "a ChildOf's parent must be a live entity of the same world";
        }
        // Bounded, in case earlier changes left a cycle above the parent that doesn't reach entity.
        std::size_t steps = 0;
        for (Entity ancestor = parent; !ancestor.isNull() && steps <= world.entityCount(); ++steps) {
            if (ancestor == entity) {
                return "a ChildOf's parent can't be the entity itself or one of its descendants";
            }
            ancestor = parentOf(ancestor);
        }
        return {};
    }

    // Checks one change and plans it; appends the change that undoes it to `undo`.
    Expected<void> plan(Entity entity, const ComponentChange& change, const std::string& where,
                        std::vector<ComponentChange>& undo) {
        const TypeInfo* type = change.type;
        if (type == nullptr || type->role != TypeRole::Component) {
            return makeError("ecs.patch.not_a_component",
                             where + (type == nullptr ? std::string("the change has no type")
                                                      : std::string(type->name) + " is not a component type"));
        }
        if (type->id == typeOf<Children>().id || type->id == typeOf<DisabledInHierarchy>().id) {
            return makeError("ecs.patch.world_owned", where + std::string(type->name) + " is kept by the World");
        }
        const ComponentIndex index = registerComponentType(*type);
        const void* before = current(entity, index);

        OwnedValue after;
        Step step = Step::Add;
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
                undo.push_back(ComponentChange::makePatch(std::move(prepared->inverse)));
                break;
            }
            case ComponentChange::Kind::Add:
                if (!change.value.hasValue() || change.value.type()->id != type->id) {
                    return makeError("reflection.patch.wrong_type",
                                     where + "the value is not a " + std::string(type->name));
                }
                after = change.value;
                undo.push_back(before != nullptr ? ComponentChange::makeAdd(OwnedValue::copyOf(*type, before))
                                                 : ComponentChange::makeRemove(*type));
                break;
            case ComponentChange::Kind::Remove:
                if (before == nullptr) {
                    return makeError("ecs.patch.missing_component",
                                     where + "the entity has no " + std::string(type->name) + " to remove");
                }
                step = Step::Remove;
                undo.push_back(ComponentChange::makeAdd(OwnedValue::copyOf(*type, before)));
                break;
        }

        if (step != Step::Remove) {
            if (const std::string reason = refusal(entity, index, after.get()); !reason.empty()) {
                return makeError("ecs.patch.refused_value", where + reason);
            }
        }
        record(entity, index, step != Step::Remove, after);
        planned.push_back(Planned{.entity = entity, .type = index, .step = step, .value = std::move(after)});
        return {};
    }

    // Makes the planned changes in order.
    void commit() {
        for (Planned& change : planned) {
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
                case Step::Add: {
                    [[maybe_unused]] const void* added =
                        world.add(change.entity, componentType(change.type), change.value.get());
                    ITH_ASSERT_MSG(added != nullptr, "ecs: applyPatches: a checked value was refused");
                    break;
                }
                case Step::Remove: {
                    [[maybe_unused]] const bool removed = world.removeComponent(change.entity, change.type);
                    ITH_ASSERT_MSG(removed, "ecs: applyPatches: a hook removed a component that a later change "
                                            "removes");
                    break;
                }
            }
        }
    }
};

} // namespace ith::detail

namespace ith {

Expected<std::vector<EntityPatch>> World::applyPatches(std::span<const EntityPatch> patches) {
    if (!canChangeStructure()) {
        return makeError("ecs.patch.iterating", "applyPatches while a query iterates or a system runs");
    }
    detail::PatchApplier applier{.world = *this};
    std::vector<EntityPatch> inverse;
    inverse.reserve(patches.size());
    for (std::size_t patchIndex = 0; patchIndex < patches.size(); ++patchIndex) {
        const EntityPatch& patch = patches[patchIndex];
        if (findRecord(patch.entity) == nullptr) {
            return makeError("ecs.patch.stale_entity",
                             "entity patch " + std::to_string(patchIndex) + ": the entity is not alive in this world");
        }
        EntityPatch& undo = inverse.emplace_back(EntityPatch{.entity = patch.entity});
        for (std::size_t changeIndex = 0; changeIndex < patch.changes.size(); ++changeIndex) {
            const std::string where =
                "entity patch " + std::to_string(patchIndex) + ", change " + std::to_string(changeIndex) + ": ";
            if (Expected<void> planned = applier.plan(patch.entity, patch.changes[changeIndex], where, undo.changes);
                !planned) {
                return makeUnexpected(std::move(planned).error());
            }
        }
        std::ranges::reverse(undo.changes);
    }
    applier.commit();
    std::ranges::reverse(inverse);
    return inverse;
}

} // namespace ith
