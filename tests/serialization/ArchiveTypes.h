#pragma once

// Reflected types for the serialization tests, next to those of tests/reflection: a type that
// contains itself through a vector, for nesting limits; a type much larger in memory than in its
// encoding, for allocation budgets; and containers with non-empty defaults. Generated into the test library
// ith_test_archive (module name "test_archive").

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

} // namespace ith::test::archive
