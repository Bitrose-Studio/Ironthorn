#pragma once

// Reflected types for the serialization tests, next to those of tests/reflection: a type that
// contains itself through a vector, for nesting limits; a type much larger in memory than in its
// encoding, for allocation budgets; containers with non-empty defaults; and entity handles at
// every depth, for the entity walker and mapping (ADR-0032). Generated into the test library
// ith_test_archive (module name "test_archive").

#include "ith/foundation/Entity.h"
#include "ith/reflection/Annotations.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ith::test::archive {

// A tree: nests as deep as the data does.
struct Node {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::int32_t value = 0;
    ITH_FIELD()
    std::vector<Node> children;
};

// Four bytes in an archive, over 4 KiB in memory.
struct Bulky {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::int32_t value = 0;
    std::array<std::uint8_t, 4096> scratch{}; // not reflected
};

// Containers whose defaults are not empty.
struct Defaults {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::vector<Node> nodes = {Node{.value = 5}};
    ITH_FIELD()
    std::optional<Node> maybe = Node{.value = 6};
};

// An entity next to plain data.
struct Link {
    ITH_TYPE(Struct)
    ITH_FIELD()
    ith::Entity target;
    ITH_FIELD()
    std::int32_t weight = 0;
};

// Entities in every container, in nested structs, in a Transient field and in a field that isn't
// reflected.
struct Squad {
    ITH_TYPE(Struct)
    ITH_FIELD()
    ith::Entity leader;
    ITH_FIELD()
    std::vector<ith::Entity> members;
    ITH_FIELD()
    std::array<ith::Entity, 2> pair{};
    ITH_FIELD()
    std::optional<ith::Entity> target;
    ITH_FIELD()
    Link link;
    ITH_FIELD()
    std::vector<std::optional<Link>> links;
    ITH_FIELD(Transient)
    ith::Entity cached;
    ith::Entity hidden; // not reflected, so neither remapped nor saved
};

// Plain data only: its record says it holds no entity.
struct NoEntities {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::vector<std::int32_t> values;
    ITH_FIELD()
    std::optional<float> scale;
};

} // namespace ith::test::archive
