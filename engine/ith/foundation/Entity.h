#pragma once

// ith::Entity: the handle of an entity in a World (ADR-0014, ADR-0032).
//
//   const ith::Entity player = world.spawn(Position{}, Health{});
//   if (world.isAlive(player)) { ... }
//
// Worlds live in the ecs module. The handle lives here, below reflection, so that reflection can
// describe Entity fields (TypeKind::Entity) and remap them (ith/reflection/Entities.h).
//
// A handle is 64 bits: a 32-bit slot index, a 24-bit generation and an 8-bit world id. When an
// entity is despawned its slot's generation goes up, so every handle to it becomes stale: a
// stale handle fails every lookup and never refers to a later entity in the same slot. A slot
// whose generation would wrap is retired and never reused. World ids start at 1, so the default
// handle (all zero bits) is the null handle of every world.
//
// Handles are runtime-only: their bits are never saved, sent over the network or used by editor
// undo. Persistent identity is the StableId component (ADR-0032). An archive writes an Entity
// field only through an entity mapping, as the stable id it maps to, and refuses it without one.
//
// An Entity is a plain value: trivially copyable, comparable and hashable through bits().

#include <cstdint>

namespace ith {

class Entity {
public:
    // Bits of each part of a handle.
    static constexpr std::uint32_t kIndexBits = 32;
    static constexpr std::uint32_t kGenerationBits = 24;
    static constexpr std::uint32_t kWorldBits = 8;
    // The largest generation a slot reaches before it is retired.
    static constexpr std::uint32_t kMaxGeneration = (1u << kGenerationBits) - 1;

    // The null handle. It refers to no entity in any world.
    constexpr Entity() noexcept = default;

    // A handle from its parts. `generation` keeps its low 24 bits. Worlds create handles; tests
    // and serializers of runtime state use this to build one.
    [[nodiscard]] static constexpr Entity fromParts(std::uint32_t index, std::uint32_t generation,
                                                    std::uint8_t worldId) noexcept {
        return fromBits(static_cast<std::uint64_t>(index) |
                        (static_cast<std::uint64_t>(generation & kMaxGeneration) << kIndexBits) |
                        (static_cast<std::uint64_t>(worldId) << (kIndexBits + kGenerationBits)));
    }

    // A handle from the value bits() returned.
    [[nodiscard]] static constexpr Entity fromBits(std::uint64_t bits) noexcept {
        Entity entity;
        entity.m_bits = bits;
        return entity;
    }

    // The whole handle as one integer, e.g. as a hash map key.
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept { return m_bits; }

    // The slot in the world's entity table.
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return static_cast<std::uint32_t>(m_bits); }

    // How many times the slot was reused before this entity.
    [[nodiscard]] constexpr std::uint32_t generation() const noexcept {
        return static_cast<std::uint32_t>(m_bits >> kIndexBits) & kMaxGeneration;
    }

    // The id of the world that created the handle; 0 only for the null handle.
    [[nodiscard]] constexpr std::uint8_t worldId() const noexcept {
        return static_cast<std::uint8_t>(m_bits >> (kIndexBits + kGenerationBits));
    }

    // True for the null handle.
    [[nodiscard]] constexpr bool isNull() const noexcept { return m_bits == 0; }

    friend constexpr bool operator==(Entity a, Entity b) noexcept = default;

private:
    std::uint64_t m_bits = 0;
};

} // namespace ith
