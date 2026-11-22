#include "ith/editor_core/EditorSession.h"

#include "ith/ecs/StableId.h"

namespace ith {

namespace {

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMapping::toId has
Uuid toStableId(void* context, Entity entity) {
    const World& world = static_cast<const EditorDocument*>(context)->world();
    const StableId* id = entity.worldId() == world.id() ? world.get<const StableId>(entity) : nullptr;
    return id != nullptr ? id->id : Uuid();
}

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMapping::toEntity has
Entity fromStableId(void* context, const Uuid& id) {
    return static_cast<const EditorDocument*>(context)->world().findByStableId(id);
}

} // namespace

EditorSession::EditorSession(const TypeRegistry& types)
    : m_types(&types), m_mapping{.toId = &toStableId, .toEntity = &fromStableId, .context = &m_document} {
    registerEditCommands(m_commands);
}

Expected<serialization::Value> EditorSession::execute(std::string_view id, const serialization::Value& args) {
    return m_commands.execute(*this, id, args);
}

} // namespace ith
