#include "ith/editor_core/EditorDocument.h"

#include "ith/foundation/Assert.h"

#include <utility>

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
