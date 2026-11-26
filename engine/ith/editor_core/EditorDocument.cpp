#include "ith/editor_core/EditorDocument.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Query.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Entity.h"

#include <utility>
#include <vector>

namespace ith {

EditorDocument::EditorDocument() : m_levelId(Uuid::generate()) {
    m_history.reset();
}

void EditorDocument::reset() {
    ITH_ASSERT_MSG(!m_transactions.isOpen(), "editor: the document was reset while a transaction is open");
    m_world.clear();
    adopt(Uuid::generate(), {});
}

void EditorDocument::adopt(const Uuid& levelId, std::string path) {
    ITH_ASSERT_MSG(!m_transactions.isOpen(), "editor: a level was opened while a transaction is open");
    // WorldTransform is Transient, so a loaded level has none; give one to every placed entity, as
    // ith.entity.create does, so transform propagation and the viewport see it. Outside the
    // history: the history starts here.
    std::vector<Entity> unplaced;
    m_world.query<const LocalTransform, Without<WorldTransform>, IncludeDisabled>().forEach(
        [&unplaced](Entity entity, const LocalTransform&) { unplaced.push_back(entity); });
    for (const Entity entity : unplaced) {
        m_world.add(entity, WorldTransform{});
    }
    m_history.reset();
    m_selection.clear();
    m_levelId = levelId;
    m_path = std::move(path);
    m_changes.bumpStructure();
    m_changes.bumpSelection();
    m_changes.bumpDocument();
}

void EditorDocument::setPath(std::string path) {
    m_path = std::move(path);
    m_changes.bumpDocument();
}

void EditorDocument::setSaving(bool saving) noexcept {
    m_saving = saving;
    m_changes.bumpDocument();
}

void EditorDocument::setOpening(bool opening) noexcept {
    m_opening = opening;
    m_changes.bumpDocument();
}

} // namespace ith
