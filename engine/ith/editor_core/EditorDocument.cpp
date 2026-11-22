#include "ith/editor_core/EditorDocument.h"

#include "ith/foundation/Assert.h"

namespace ith {

EditorDocument::EditorDocument() : m_levelId(Uuid::generate()) {
    m_history.reset();
}

void EditorDocument::reset() {
    ITH_ASSERT_MSG(!m_transactions.isOpen(), "editor: the document was reset while a transaction is open");
    m_world.clear();
    m_history.reset();
    m_selection.clear();
    m_levelId = Uuid::generate();
    m_changes.bumpStructure();
    m_changes.bumpSelection();
    m_changes.bumpDocument();
}

} // namespace ith
