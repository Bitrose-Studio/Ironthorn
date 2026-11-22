#pragma once

// ith::Commands: structural changes recorded now and applied later (ADR-0014 rule 8).
//
//   ith::Commands commands(world);
//   const ith::Entity bullet = commands.spawn(Position{}, Velocity{.x = 10.0f});
//   commands.add(target, Damage{.amount = 5});
//   commands.remove<Shield>(target);
//   commands.despawn(spent);
//   world.apply(commands); // in recording order; the buffer is empty afterwards
//
// A command buffer records spawn, despawn, add and remove, and despawnRecursive and placeBefore
// (Hierarchy.h), for one World. Nothing changes until World::apply carries the commands out, in
// the order they were recorded; each then behaves like the World function of the same name (a
// command on a stale handle does nothing). Systems get a buffer from the schedule (Schedule.h),
// which applies it at the end of the system's stage.
//
// spawn returns the new entity's handle at once: the handle is reserved in the world, and the
// entity is alive from the moment its spawn command is applied. Until then the handle fails every
// lookup, but later commands, here or in a buffer applied afterwards, may already use it. A buffer
// destroyed with commands it never applied drops them: their values are destroyed and their
// reserved handles stay stale for good.
//
// Values are copied or moved into the buffer when they are recorded, so recording a value stored
// in the world is safe. The buffer keeps its memory after apply, so a buffer reused every frame
// stops allocating once it has seen its largest frame.
//
// Threads: one buffer is used by one thread at a time. Jobs may record into their own buffers at
// the same time, also spawns, while nothing changes the world's structure, as during a system's
// run. For a deterministic result, give each job (or each range of a parallelFor) its own buffer
// and append them, in index order, to the system's buffer: append keeps the order. The handles a
// spawn reserves then depend on timing, but nothing else does: handles are runtime-only and never
// decide the order of anything.
//
// A buffer refers to its World and must not outlive it. Movable, not copyable.

#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/Entity.h"
#include "ith/ecs/Query.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace ith {

class World;

class Commands {
public:
    // An empty buffer for `world`.
    explicit Commands(World& world) noexcept : m_world(&world) {}

    // Drops the commands that were not applied (see the header comment).
    ~Commands();

    // Takes the other buffer's commands and memory; `other` is left empty, for the same world.
    Commands(Commands&& other) noexcept;
    Commands& operator=(Commands&&) = delete;
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;

    // Records the spawn of an entity with the given components, each of a distinct component type,
    // and returns its reserved handle. A type without the Component role asserts; nothing is then
    // recorded and the null handle is returned.
    template <typename... Components>
    Entity spawn(Components&&... components);

    // Records the despawn of `entity`.
    void despawn(Entity entity);

    // Records despawning `entity` and everything below it (World::despawnRecursive).
    void despawnRecursive(Entity entity);

    // Records moving `entity` before `sibling` (World::placeBefore).
    void placeBefore(Entity entity, Entity sibling);

    // Records giving `entity` the value as its T, replacing a T it has by then.
    template <typename T>
    void add(Entity entity, T&& value);

    // Records removing the entity's T.
    template <typename T>
    void remove(Entity entity);

    // Moves the commands of `other`, a buffer of the same world (asserted), to the end of this one,
    // in their order; `other` is left empty. Values move through their type records.
    void append(Commands& other);

    // The number of recorded commands.
    [[nodiscard]] std::size_t size() const noexcept { return m_commands.size(); }

    [[nodiscard]] bool empty() const noexcept { return m_commands.empty(); }

private:
    friend class World;

    enum class Kind : std::uint8_t {
        Spawn,
        Despawn,
        Add,
        Remove,
        DespawnRecursive,
        PlaceBefore,
        LinkChild, // the hierarchy hooks: append `entity` to its parent's children
    };

    struct Command {
        Kind kind = Kind::Despawn;
        Entity entity;
        Entity other;                                       // PlaceBefore: the sibling
        detail::ComponentIndex type = detail::kNoComponent; // Remove
        std::uint32_t firstValue = 0;                       // Spawn and Add: the first of their values
        std::uint32_t valueCount = 0;
    };

    // A recorded component value in the buffer's memory.
    struct Value {
        detail::ComponentIndex type = detail::kNoComponent;
        void* memory = nullptr;
    };

    struct Block {
        std::unique_ptr<std::byte[]> memory; // NOLINT(modernize-avoid-c-arrays): raw bytes, not zero-filled
        std::size_t size = 0;
        std::size_t used = 0;
    };

    // Uninitialized memory for one value, valid until reset().
    void* allocate(std::size_t size, std::size_t alignment);

    template <typename T>
    void storeValue(T&& value) {
        using Component = std::remove_cvref_t<T>;
        void* memory = allocate(sizeof(Component), alignof(Component));
        ::new (memory) Component(std::forward<T>(value));
        m_values.push_back({detail::componentIndexOf<Component>(), memory});
    }

    Entity reserveEntity();

    // Empties the buffer and keeps its memory. Precondition: every value is destroyed or moved out.
    void reset() noexcept;

    // Destroys the values and hands back the reserved handles of unapplied spawns.
    void discard() noexcept;

    World* m_world;
    std::vector<Command> m_commands;
    std::vector<Value> m_values;
    std::vector<Block> m_blocks;
    std::size_t m_block = 0; // the block allocate() tries first
};

template <typename... Components>
Entity Commands::spawn(Components&&... components) {
    static_assert(detail::kDistinctTypes<std::remove_cvref_t<Components>...>,
                  "spawn takes at most one component of each type");
    const std::array<detail::ComponentIndex, sizeof...(Components)> types{
        detail::componentIndexOf<std::remove_cvref_t<Components>>()...};
    if (std::ranges::find_if(types, [](detail::ComponentIndex type) { return type == detail::kNoComponent; }) !=
        types.end()) {
        return {}; // a type without the Component role; registering it asserted
    }
    const Entity entity = reserveEntity();
    const auto first = static_cast<std::uint32_t>(m_values.size());
    (storeValue(std::forward<Components>(components)), ...);
    m_commands.push_back({.kind = Kind::Spawn,
                          .entity = entity,
                          .firstValue = first,
                          .valueCount = static_cast<std::uint32_t>(sizeof...(Components))});
    return entity;
}

template <typename T>
void Commands::add(Entity entity, T&& value) {
    if (detail::componentIndexOf<std::remove_cvref_t<T>>() == detail::kNoComponent) {
        return; // registering the type asserted
    }
    const auto first = static_cast<std::uint32_t>(m_values.size());
    storeValue(std::forward<T>(value));
    m_commands.push_back({.kind = Kind::Add, .entity = entity, .firstValue = first, .valueCount = 1});
}

template <typename T>
void Commands::remove(Entity entity) {
    const detail::ComponentIndex type = detail::componentIndexOf<std::remove_const_t<T>>();
    if (type != detail::kNoComponent) {
        m_commands.push_back({.kind = Kind::Remove, .entity = entity, .type = type});
    }
}

} // namespace ith
