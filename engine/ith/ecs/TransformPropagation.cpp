#include "ith/ecs/TransformPropagation.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ith {
namespace {

// parent * toMatrix(local) for an affine parent (last row 0, 0, 0, 1), without the products by
// that row.
Mat4 composeAffine(const Mat4& parent, const Transform& local) noexcept {
    const Mat3 rotation = glm::mat3_cast(local.rotation);
    const Vec4& x = parent[0];
    const Vec4& y = parent[1];
    const Vec4& z = parent[2];
    Mat4 world;
    for (int axis = 0; axis < 3; ++axis) {
        const Vec3 column = rotation[axis] * local.scale[axis];
        world[axis] = (x * column.x) + (y * column.y) + (z * column.z);
    }
    world[3] = (x * local.translation.x) + (y * local.translation.y) + (z * local.translation.z) + parent[3];
    return world;
}

} // namespace

void propagateTransforms(World& world) {
    // Level by level, breadth first, from the roots: siblings are usually stored next to each
    // other, so a level reads its components mostly in storage order. Each entity is looked up
    // once; the columns of the last archetype seen are reused.
    struct Pending {
        Entity entity;
        std::uint32_t parent = 0; // the index of the parent's matrix in the previous level
    };

    std::vector<Pending> level;
    std::vector<Pending> next;
    std::vector<Mat4> parentWorlds{Mat4(1.0f)}; // the roots' parent: the identity
    std::vector<Mat4> worlds;
    world.forEachRoot([&](Entity root) { level.push_back({.entity = root, .parent = 0}); });

    const detail::ComponentIndex localType = detail::componentIndexOf<LocalTransform>();
    const detail::ComponentIndex worldType = detail::componentIndexOf<WorldTransform>();
    const detail::ComponentIndex childrenType = detail::componentIndexOf<Children>();
    detail::ArchetypeIndex cached = detail::kNoArchetype;
    std::size_t localColumn = detail::Archetype::kNoColumn;
    std::size_t worldColumn = detail::Archetype::kNoColumn;
    std::size_t childrenColumn = detail::Archetype::kNoColumn;

    while (!level.empty()) {
        next.clear();
        worlds.clear();
        worlds.reserve(level.size());
        for (std::size_t index = 0; index < level.size(); ++index) {
            const Pending& current = level[index];
            const World::EntityRecord* record = world.findRecord(current.entity);
            Mat4& matrix = worlds.emplace_back(parentWorlds[current.parent]);
            if (record == nullptr) {
                continue;
            }
            detail::Archetype& archetype = *world.m_archetypes[record->archetype];
            if (record->archetype != cached) {
                cached = record->archetype;
                localColumn = archetype.columnOf(localType);
                worldColumn = archetype.columnOf(worldType);
                childrenColumn = archetype.columnOf(childrenType);
            }
            const std::uint32_t row = record->row;
            if (localColumn != detail::Archetype::kNoColumn) {
                matrix =
                    composeAffine(parentWorlds[current.parent],
                                  static_cast<const LocalTransform*>(archetype.column(localColumn).at(row))->value);
            }
            if (worldColumn != detail::Archetype::kNoColumn) {
                static_cast<WorldTransform*>(archetype.column(worldColumn).at(row))->matrix = matrix;
            }
            if (childrenColumn != detail::Archetype::kNoColumn) {
                const auto& children = static_cast<const Children*>(archetype.column(childrenColumn).at(row))->entities;
                for (const Entity child : children) {
                    next.push_back({.entity = child, .parent = static_cast<std::uint32_t>(index)});
                }
            }
        }
        std::swap(level, next);
        std::swap(parentWorlds, worlds);
    }
}

namespace detail {

void runTransformPropagation(const SystemContext& context) {
    propagateTransforms(*context.m_world); // covered by the system's declared access
}

} // namespace detail

SystemDescriptor transformPropagationSystem() {
    return {
        .name = "ith::propagate_transforms",
        .stage = Stage::PostUpdate,
        .access = accessTo<const LocalTransform, WorldTransform, const Children>(),
        .run = &detail::runTransformPropagation,
    };
}

} // namespace ith
