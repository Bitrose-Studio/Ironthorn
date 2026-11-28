#include "ith/ecs/WorldCopy.h"

#include "ith/ecs/StableId.h"
#include "ith/foundation/Assert.h"
#include "ith/reflection/Entities.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace ith {
namespace {

// Where a source handle goes in the copy: the same slot and generation under the target's id.
struct CopyMap {
    const World* source = nullptr;
    std::uint8_t target = 0;
};

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMap::function has
Entity toCopy(void* context, Entity entity) {
    const auto& map = *static_cast<const CopyMap*>(context);
    // Checked in this order so that a handle of another world doesn't assert.
    if (entity.isNull() || entity.worldId() != map.source->id() || !map.source->isAlive(entity)) {
        return {};
    }
    return Entity::fromParts(entity.index(), entity.generation(), map.target);
}

bool isLeftOut(const TypeInfo& type, const WorldCopyOptions& options) noexcept {
    return options.excludeEditorOnly && hasFlags(type.flags, ReflectionFlags::EditorOnly);
}

// Copy-constructs a value of `type` and remaps the entities in it.
void copyValue(const TypeInfo& type, void* destination, const void* source, const EntityMap& map) {
    if (type.triviallyCopyable) {
        std::memcpy(destination, source, type.size);
    } else {
        type.copy(destination, source);
    }
    remapEntities(type, destination, map);
}

} // namespace

bool copyWorld(const World& source, World& target, const WorldCopyOptions& options) {
    if (&source == &target) {
        ITH_ASSERT_MSG(false, "copyWorld: a world can't be copied into itself");
        return false;
    }
    const bool sourceIdle = source.m_iterationDepth == 0 && source.m_hookDepth == 0;
    ITH_ASSERT_MSG(sourceIdle, "copyWorld: the source is being iterated or runs a hook");
    const bool fresh = target.m_records.empty() && target.m_freeCursor.load(std::memory_order_relaxed) == 0 &&
                       target.m_resources.empty() && target.m_lastRuns.empty() && target.m_iterationDepth == 0;
    ITH_ASSERT_MSG(fresh, "copyWorld: the target must be a fresh world");
    if (!sourceIdle || !fresh) {
        return false;
    }

    CopyMap copyMap{.source = &source, .target = target.m_id};
    const EntityMap map{.function = &toCopy, .context = &copyMap};

    // The slots, with their generations and root links; the entities fill them below. Free slots,
    // reserved ones included, stay free.
    target.m_records.resize(source.m_records.size());
    for (std::size_t index = 0; index < source.m_records.size(); ++index) {
        World::EntityRecord& record = target.m_records[index];
        const World::EntityRecord& from = source.m_records[index];
        record.generation = from.generation;
        record.isRoot = from.isRoot && from.archetype != detail::kNoArchetype;
        record.previousRoot = from.previousRoot;
        record.nextRoot = from.nextRoot;
    }
    target.m_freeSlots = source.m_freeSlots;
    target.m_freeCursor.store(static_cast<std::int64_t>(target.m_freeSlots.size()), std::memory_order_relaxed);
    target.m_firstRoot = source.m_firstRoot;
    target.m_lastRoot = source.m_lastRoot;

    const detail::ComponentIndex stableIdType = detail::componentIndexOf<StableId>();
    std::vector<detail::ComponentIndex> types;
    for (const auto& archetypePointer : source.m_archetypes) {
        detail::Archetype& from = *archetypePointer;
        if (from.size() == 0) {
            continue;
        }
        types.clear();
        for (const detail::ComponentIndex type : from.types()) {
            if (!isLeftOut(detail::componentType(type), options)) {
                types.push_back(type);
            }
        }
        const detail::ArchetypeIndex archetypeIndex = target.findOrCreateArchetype(types);
        detail::Archetype& to = *target.m_archetypes[archetypeIndex];
        // The source column of each target column.
        std::vector<std::size_t> sourceColumns;
        sourceColumns.reserve(types.size());
        for (const detail::ComponentIndex type : types) {
            sourceColumns.push_back(from.columnOf(type));
        }
        const std::size_t stableIdColumn = to.columnOf(stableIdType);
        for (std::uint32_t row = 0; row < from.size(); ++row) {
            const Entity entity = toCopy(&copyMap, from.entities()[row]);
            const std::uint32_t copyRow = to.pushRow(entity, target.m_clock.tick);
            for (std::size_t column = 0; column < types.size(); ++column) {
                copyValue(detail::componentType(types[column]), to.column(column).at(copyRow),
                          from.column(sourceColumns[column]).at(row), map);
            }
            World::EntityRecord& record = target.m_records[entity.index()];
            record.archetype = archetypeIndex;
            record.row = copyRow;
            ++target.m_entityCount;
            if (stableIdColumn != detail::Archetype::kNoColumn) {
                const auto& id = *static_cast<const StableId*>(to.column(stableIdColumn).at(copyRow));
                target.m_stableIds.emplace(id.id, entity);
            }
        }
    }

    for (const auto& entry : source.m_resources) {
        const auto& value = entry.second;
        const TypeInfo& type = *value.get_deleter().type;
        if (isLeftOut(type, options)) {
            continue;
        }
        void* memory = target.prepareResource(type);
        copyValue(type, memory, value.get(), map);
    }
    ++target.m_structureGeneration;
    return true;
}

} // namespace ith
