#pragma once

// Component hooks (ADR-0014 rule 10, ADR-0030): engine functions that run as a component type's
// values are added, replaced and removed, to keep invariants such as the hierarchy.
//
//   void onOwnerRemoved(ith::HookContext& context) {
//       const auto* owner = context.value<const game::Owner>();
//       if (auto* log = context.resource<game::OwnershipLog>()) { log->released.push_back(owner->id); }
//   }
//
//   ith::setComponentHooks<game::Owner>({.onRemove = &onOwnerRemoved}); // at startup, once
//
// Hooks belong to a component type, once per process: every World runs them, Play In Editor
// copies included. They are set once, before any World stores the type; setting them again, or
// after the type was first stored, asserts and changes nothing. Hooks are engine invariants:
// gameplay reacts to changes through systems (ADR-0016 rule 4). A hook is a plain function
// without state; state that a hook needs lives in a world resource.
//
// When they run:
// - onAdd: the entity gains a type it didn't have; the new value is in place.
// - onInsert: after every add or replace, after onAdd; the new value is in place.
// - onReplace: before a value is overwritten (add on an entity that has the type) or removed;
//   the old value is still in place.
// - onRemove: before the value is removed, also by despawn and clear(); the value is still in
//   place.
// A spawn runs onAdd for each of its components, then onInsert for each, in the order the
// components were given (the order of the arguments, or of the values a spawn command recorded).
// Despawn runs onReplace for every component of the entity, then onRemove for every one, while
// the entity is still whole, sorted by stable type id. clear() does the same for each entity,
// archetype by archetype, before anything is destroyed. The World's destructor runs no hooks.
//
// What a hook may do: read and write components and resources of its world, and record commands
// in context.commands(). It can't change the world's structure directly (spawn, despawn, add,
// remove and clear are refused while a hook runs, as inside a query). The commands it records are
// applied right after the change that ran the hook, before the caller gets control back; commands
// recorded by the hooks those commands run are applied after them in turn, depth-first. Nesting
// deeper than kMaxHookDepth is fatal: it means hooks that keep triggering each other. Within
// World::apply, each command's hook commands are applied before the next command.

#include "ith/ecs/Commands.h"
#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/Entity.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Assert.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include <cstdint>
#include <type_traits>

namespace ith {

// How deep hook commands may nest: commands recorded by hooks that commands recorded by hooks ran.
inline constexpr std::uint32_t kMaxHookDepth = 64;

class HookContext;

// A hook: a plain function without state.
using ComponentHook = void (*)(HookContext& context);

// The hooks of one component type; a null hook doesn't run.
struct ComponentHooks {
    ComponentHook onAdd = nullptr;
    ComponentHook onInsert = nullptr;
    ComponentHook onReplace = nullptr;
    ComponentHook onRemove = nullptr;
};

// What a hook works with: the entity, the component's type and value, the world's data and a
// command buffer. Valid only during the hook's run.
class HookContext {
public:
    HookContext(const HookContext&) = delete;
    HookContext& operator=(const HookContext&) = delete;
    HookContext(HookContext&&) = delete;
    HookContext& operator=(HookContext&&) = delete;
    ~HookContext() = default;

    // The entity whose component is changing.
    [[nodiscard]] Entity entity() const noexcept { return m_entity; }

    // The reflection record of the component type whose hook runs.
    [[nodiscard]] const TypeInfo& type() const noexcept { return *m_type; }

    // The component's value: the new value in onAdd and onInsert, the old one in onReplace and
    // onRemove.
    [[nodiscard]] const void* value() const noexcept { return m_value; }

    // The value as T, which must be the hook's component type (asserted; null otherwise). T may be
    // const; a non-const T marks the value changed, and a non-const T of an immutable type asserts
    // and gives null.
    template <typename T>
    [[nodiscard]] T* value();

    // World::get, World::has and World::isAlive on the hook's world. A non-const get of an
    // immutable type asserts and gives null.
    template <typename T>
    [[nodiscard]] T* get(Entity entity) {
        return m_world->get<T>(entity);
    }

    template <typename T>
    [[nodiscard]] bool has(Entity entity) const {
        return m_world->has<T>(entity);
    }

    [[nodiscard]] bool isAlive(Entity entity) const { return m_world->isAlive(entity); }

    // World::resource on the hook's world. Resources may also be inserted and removed (they are not
    // structural changes); declare a typed resource for state a hook needs.
    template <typename T>
    [[nodiscard]] T* resource() {
        return m_world->resource<T>();
    }

    // The buffer for commands applied right after the change that ran the hook.
    [[nodiscard]] Commands& commands() noexcept { return *m_commands; }

private:
    friend class World;

    HookContext(World& world, Commands& commands, Entity entity, const TypeInfo& type, void* value) noexcept
        : m_world(&world), m_commands(&commands), m_entity(entity), m_type(&type), m_value(value) {}

    World* m_world;
    Commands* m_commands;
    Entity m_entity;
    const TypeInfo* m_type;
    void* m_value;
};

// Sets the hooks of a component type for the whole process (see the header comment). Returns
// false, changing nothing, when `type` is not a component type, its hooks were set before, or a
// World already stored it; each of these asserts.
bool setComponentHooks(const TypeInfo& type, const ComponentHooks& hooks);

template <typename T>
bool setComponentHooks(const ComponentHooks& hooks) {
    return setComponentHooks(typeOf<std::remove_cvref_t<T>>(), hooks);
}

template <typename T>
T* HookContext::value() {
    const bool sameType = typeOf<std::remove_const_t<T>>().id == m_type->id;
    ITH_ASSERT_MSG(sameType, "ecs: HookContext::value names another type than the hook's component");
    if (!sameType) {
        return nullptr;
    }
    if constexpr (!std::is_const_v<T>) {
        if (!detail::isWritableComponent(*m_type)) {
            return nullptr;
        }
        m_world->markChanged(m_entity, detail::componentIndexOf<T>());
    }
    return static_cast<T*>(m_value);
}

} // namespace ith
