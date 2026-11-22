#include "ith/ecs/TransformPropagation.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

constexpr std::size_t kNoColumn = detail::Archetype::kNoColumn;

} // namespace

namespace detail {

// Propagation in three steps:
// 1. Find the entities whose placement changed in the run's window: a LocalTransform added or
//    written, a ChildOf added or replaced, or a WorldTransform added. Losing a parent or a
//    LocalTransform leaves a tick too (World::markPlacementChanged). Writes to WorldTransform are
//    not looked at, so propagations don't trigger each other.
// 2. Keep the ones without a changed ancestor: their subtrees cover the rest. When at least half
//    of the entities looked at changed, start from every root instead.
// 3. Walk level by level, breadth first, from those entities, as a full propagation does from the
//    roots. A parent outside the changed subtrees is up to date, so its WorldTransform is the
//    starting matrix, and the matrices come out the same as from a full propagation.
struct TransformPropagation {
    // An entity to propagate, with the index of its parent's world matrix in the previous level.
    struct Pending {
        Entity entity;
        std::uint32_t parent = 0;
    };

    // An entity whose placement changed, and its parent (null for a root).
    struct Change {
        Entity entity;
        Entity parent;
    };

    // Calls `found(entity, parent)` for each entity whose placement changed in the world's current
    // run; returns how many entities with LocalTransform, WorldTransform or ChildOf it looked at.
    template <typename Found>
    static std::size_t scan(World& world, Found&& found) {
        const ChangeClock clock = world.m_clock;
        const auto newer = [&clock](const ChangeTicks* ticks, std::size_t row) {
            return ticks != nullptr && isNewer(ticks[row].changed, clock.lastRun, clock.tick);
        };
        const auto added = [&clock](const ChangeTicks* ticks, std::size_t row) {
            return ticks != nullptr && isNewer(ticks[row].added, clock.lastRun, clock.tick);
        };
        const auto ticksOf = [](Archetype& archetype, std::size_t column) -> const ChangeTicks* {
            return column != kNoColumn ? archetype.column(column).ticks() : nullptr;
        };
        std::size_t scanned = 0;
        for (const std::unique_ptr<Archetype>& pointer : world.m_archetypes) {
            Archetype& archetype = *pointer;
            const std::size_t localColumn = archetype.columnOf(componentIndexOf<LocalTransform>());
            const std::size_t worldColumn = archetype.columnOf(componentIndexOf<WorldTransform>());
            const std::size_t linkColumn = archetype.columnOf(componentIndexOf<ChildOf>());
            if (localColumn == kNoColumn && worldColumn == kNoColumn && linkColumn == kNoColumn) {
                continue;
            }
            const ChangeTicks* localTicks = ticksOf(archetype, localColumn);
            const ChangeTicks* worldTicks = ticksOf(archetype, worldColumn);
            const ChangeTicks* linkTicks = ticksOf(archetype, linkColumn);
            const auto* links =
                linkColumn != kNoColumn ? static_cast<const ChildOf*>(archetype.column(linkColumn).data()) : nullptr;
            const std::span<const Entity> entities = archetype.entities();
            scanned += entities.size();
            for (std::size_t row = 0; row < entities.size(); ++row) {
                if (newer(localTicks, row) || newer(linkTicks, row) || added(worldTicks, row)) {
                    found(entities[row], links != nullptr ? links[row].parent : Entity());
                }
            }
        }
        return scanned;
    }

    // The world matrix of `parent` (identity for null), which no change made stale: the nearest
    // WorldTransform at or above it, composed down with the local transforms of the ancestors
    // below that have none.
    static Mat4 parentWorld(World& world, Entity parent, std::vector<Entity>& chain) {
        Mat4 matrix(1.0f);
        chain.clear();
        for (Entity ancestor = parent; !ancestor.isNull(); ancestor = world.parentOf(ancestor)) {
            if (const auto* stored = static_cast<const WorldTransform*>(
                    world.findComponent(ancestor, componentIndexOf<WorldTransform>()))) {
                matrix = stored->matrix;
                break;
            }
            chain.push_back(ancestor);
        }
        for (std::size_t index = chain.size(); index-- > 0;) {
            if (const auto* local = static_cast<const LocalTransform*>(
                    world.findComponent(chain[index], componentIndexOf<LocalTransform>()))) {
                matrix = composeAffine(matrix, local->value);
            }
        }
        return matrix;
    }

    // Fills the first level with the changed entities that have no changed ancestor, and each
    // one's parent matrix.
    static void startAtChanges(World& world, std::size_t changeCount, std::vector<Pending>& level,
                               std::vector<Mat4>& parentWorlds) {
        std::vector<Change> changes;
        changes.reserve(changeCount);
        std::vector<std::uint8_t> changedSlots(world.m_records.size(), 0); // by entity index
        scan(world, [&](Entity entity, Entity parent) {
            changes.push_back({.entity = entity, .parent = parent});
            changedSlots[entity.index()] = 1;
        });
        std::vector<Entity> chain;
        for (const Change& change : changes) {
            bool covered = false;
            for (Entity ancestor = change.parent; !ancestor.isNull() && !covered; ancestor = world.parentOf(ancestor)) {
                covered = changedSlots[ancestor.index()] != 0;
            }
            if (!covered) {
                level.push_back({.entity = change.entity, .parent = static_cast<std::uint32_t>(parentWorlds.size())});
                parentWorlds.push_back(parentWorld(world, change.parent, chain));
            }
        }
    }

    // Writes the WorldTransforms of the level's entities and everything below them, breadth first.
    // Siblings are usually stored next to each other, so a level reads its components mostly in
    // storage order. Each entity is looked up once; the columns of the last archetype seen are
    // reused.
    static void walk(World& world, std::vector<Pending>& level, std::vector<Mat4>& parentWorlds) {
        const std::uint32_t tick = world.m_clock.tick;
        std::vector<Pending> next;
        std::vector<Mat4> worlds;
        ArchetypeIndex cached = kNoArchetype;
        std::size_t localColumn = kNoColumn;
        std::size_t worldColumn = kNoColumn;
        std::size_t childrenColumn = kNoColumn;
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
                Archetype& archetype = *world.m_archetypes[record->archetype];
                if (record->archetype != cached) {
                    cached = record->archetype;
                    localColumn = archetype.columnOf(componentIndexOf<LocalTransform>());
                    worldColumn = archetype.columnOf(componentIndexOf<WorldTransform>());
                    childrenColumn = archetype.columnOf(componentIndexOf<Children>());
                }
                const std::uint32_t row = record->row;
                if (localColumn != kNoColumn) {
                    matrix =
                        composeAffine(parentWorlds[current.parent],
                                      static_cast<const LocalTransform*>(archetype.column(localColumn).at(row))->value);
                }
                if (worldColumn != kNoColumn) {
                    Column& column = archetype.column(worldColumn);
                    static_cast<WorldTransform*>(column.at(row))->matrix = matrix;
                    column.ticks(row).changed = tick;
                }
                if (childrenColumn != kNoColumn) {
                    const auto& children =
                        static_cast<const Children*>(archetype.column(childrenColumn).at(row))->entities;
                    for (const Entity child : children) {
                        next.push_back({.entity = child, .parent = static_cast<std::uint32_t>(index)});
                    }
                }
            }
            std::swap(level, next);
            std::swap(parentWorlds, worlds);
        }
    }
};

void propagateChangedTransforms(World& world) {
    std::size_t changeCount = 0;
    const std::size_t scanned =
        TransformPropagation::scan(world, [&changeCount](Entity /*entity*/, Entity /*parent*/) { ++changeCount; });
    if (changeCount == 0) {
        return;
    }
    std::vector<TransformPropagation::Pending> level;
    std::vector<Mat4> parentWorlds;
    if (changeCount * 2 >= scanned) {
        // Most of the world changed (a first propagation, a level load): a full propagation from
        // the roots is cheaper than finding the changed subtrees, and gives the same matrices.
        world.forEachRoot([&level](Entity root) { level.push_back({.entity = root, .parent = 0}); });
        parentWorlds.emplace_back(1.0f);
    } else {
        TransformPropagation::startAtChanges(world, changeCount, level, parentWorlds);
    }
    TransformPropagation::walk(world, level, parentWorlds);
}

void runTransformPropagation(const SystemContext& context) {
    propagateChangedTransforms(*context.m_world); // covered by the system's declared access
}

} // namespace detail

void propagateTransforms(World& world) {
    const detail::ChangeClock outer = world.beginRun(World::kDirectPropagationKey);
    detail::propagateChangedTransforms(world);
    world.endRun(World::kDirectPropagationKey, outer);
}

SystemDescriptor transformPropagationSystem() {
    return {
        .name = "ith::propagate_transforms",
        .stage = Stage::PostUpdate,
        .access = accessTo<const LocalTransform, WorldTransform, const ChildOf, const Children>(),
        .run = &detail::runTransformPropagation,
    };
}

} // namespace ith
