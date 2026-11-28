#pragma once

// ith::DetailsModel: the Details panel's view of the selected entities (ADR-0008 rule 5,
// ADR-0033, ADR-0037), as rows generated from reflection and the commands that change them;
// editor_ui draws the rows with a property editor per type.
//
//   ith::DetailsModel details(session);
//   // every frame:
//   details.update();
//   for (std::size_t i = 0; i < details.rows().size(); ++i) {
//       const ith::DetailsRow& row = details.rows()[i];
//       ... draw row.label and an editor for details.value(i) of type *row.type ...
//   }
//   // a checkbox, a combo or a committed text field: one command, one undo entry
//   details.setValue(i, &newValue);
//   // a slider drag: one transaction with a live preview, one undo entry
//   details.beginEdit(i);  details.previewValue(&dragged);  ...  details.commitEdit();  // or cancelEdit()
//
// Working copy. The model keeps a copy of the shown components of every selected entity and draws
// from it, never from the world. It refreshes the copy when the selection, the structure or the
// document changed, or when `values` names one of its entities (EditorChanges::touchedSince), but
// not while one of its own interactions is open: a drag is never fought by its own updates.
//
// Rows. Each shown component is a group row, then its fields (grouped by their Category attribute
// under category rows), and inside a struct, array or optional the rows of its parts, expanded and
// collapsed by their path (components, categories and structs start expanded, arrays collapsed).
// Numbers, strings, enums, entity references, Uuids, the math vectors and quaternion, asset
// references and fields with a Meta(Editor = ...) are inline: one row edits the whole value. An
// optional of an inline type is one row too (present or not, and its value).
//
// Multi-select. Every selected entity is shown, the primary last; only the components all of them
// have are listed. A row whose value differs between them is `mixed` ("Multiple Values"), and an
// edit writes the same value to all of them. A vector whose sizes differ shows no element rows.
//
// Not listed as components: Transient components (WorldTransform), the components the World keeps
// (StableId, Children, DisabledInHierarchy), and those the Outliner and the header own: EditorLabel
// (the name field) and Disabled (the Enabled check box). ChildOf is listed.
//
// Edits go through commands (ith.entity.set_property, reset_property, insert_element,
// erase_element, add_component, remove_component, set_enabled, ith.edit.rename) and interactions
// through Transactions (ADR-0037 rule 6), which refuse ReadOnly fields as the commands do. Numbers
// are clamped to their field's Min and Max before they are written (ADR-0008 rule 5).
//
// Play In Editor (ADR-0053 decisions 12 and 13). The model shows the active world's selection
// (EditorSession::active): the play selection in the game world while a session runs, with labels
// from the editor twins. It keys its cache on the active world's identity too, so Play and Stop
// always rebuild it. Value edits and drags change the game world then, recording nothing; Add and
// Remove Component, Enabled and Rename are refused by their commands. A drag still open when Stop
// destroys the game world is dropped at the next call. Main thread only.

#include "ith/editor_core/Transactions.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Uuid.h"
#include "ith/math/Types.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith {

class EditorSession;

enum class DetailsRowKind : std::uint8_t {
    Component, // a component's group
    Category,  // a Category attribute's group inside a component
    Property,  // a value inside a component
};

struct DetailsRow {
    DetailsRowKind kind = DetailsRowKind::Property;
    std::uint32_t depth = 0;
    // In ADR-0008 vocabulary: the DisplayName attribute, else the name spelled as words
    // ("despawnWithParent" is "Despawn With Parent"); "[3]" for an element.
    std::string label;
    // The Tooltip attribute, else the field's or type's name.
    std::string tooltip;
    // Component rows: the component type; Property rows: the value's type; Category rows: null.
    const TypeInfo* type = nullptr;
    // The component type the row belongs to.
    const TypeInfo* component = nullptr;
    // The last field on the path, or null (a component, an element).
    const FieldInfo* field = nullptr;
    // The path from the component: empty for a Component or Category row.
    PropertyPath path;
    // What expansion is kept by: the qualified path, or "<component>#<category>".
    std::string key;
    bool expandable = false;
    bool expanded = false;
    // A field along the path is ReadOnly: shown, not editable.
    bool readOnly = false;
    // One editor edits the whole value (see the header comment).
    bool inlineValue = false;
    // The selected entities' values differ ("Multiple Values").
    bool mixed = false;
    // Every selected entity has the default value here (no Reset to Default arrow).
    bool isDefault = true;
};

// An entity a reference field can point at, for the picker.
struct EntityChoice {
    Uuid id;
    std::string label;
};

class DetailsModel {
public:
    // `session` must outlive the model.
    explicit DetailsModel(EditorSession& session);

    DetailsModel(const DetailsModel&) = delete;
    DetailsModel& operator=(const DetailsModel&) = delete;
    DetailsModel(DetailsModel&&) = delete;
    DetailsModel& operator=(DetailsModel&&) = delete;
    // Cancels an interaction still open.
    ~DetailsModel();

    // Polls EditorChanges and refreshes the working copy and the rows when needed (see the header
    // comment). Call once per frame before drawing.
    void update();

    // The shown entities: the selection, primary last.
    [[nodiscard]] const std::vector<Uuid>& entities() const noexcept { return m_entities; }

    // The shown component types, in row order: LocalTransform first, then by label.
    [[nodiscard]] const std::vector<const TypeInfo*>& components() const noexcept { return m_components; }

    [[nodiscard]] const std::vector<DetailsRow>& rows() const noexcept { return m_rows; }

    // The row whose key is `key`, or nullopt.
    [[nodiscard]] std::optional<std::size_t> findRow(std::string_view key) const;

    // The primary entity's value of a Component or Property row in the working copy, of the row's
    // type; null for a Category row or when the primary's value doesn't reach that far.
    [[nodiscard]] const void* value(std::size_t row) const;

    // How often the working copy was refreshed, for tests.
    [[nodiscard]] std::uint64_t refreshes() const noexcept { return m_refreshes; }

    // --- The header ---

    // The primary's label; labelMixed() when the selected entities' labels differ.
    [[nodiscard]] const std::string& label() const noexcept { return m_label; }

    [[nodiscard]] bool labelMixed() const noexcept { return m_labelMixed; }

    // The name field is editable for one selected entity only, as in Unreal.
    [[nodiscard]] bool canRename() const noexcept { return m_entities.size() == 1; }

    Expected<void> rename(std::string_view label);

    // Whether the entities are enabled: nullopt when they differ.
    [[nodiscard]] std::optional<bool> enabled() const noexcept { return m_enabled; }

    Expected<void> setEnabled(bool enabled);

    // --- Expansion ---

    void setExpanded(std::size_t row, bool expanded);

    // --- One-shot edits (one command each) ---

    // Sets a Property row to `value` (of the row's type) on every shown entity, clamped to the
    // field's Min and Max: ith.entity.set_property.
    Expected<void> setValue(std::size_t row, const void* value);

    // Reset to Default (ith.entity.reset_property).
    Expected<void> resetToDefault(std::size_t row);

    // Gives an optional row a value (its type's default) or empties it.
    Expected<void> setPresent(std::size_t row, bool present);

    // A vector row: appends a default element; removes the element an element row shows; empties it.
    Expected<void> addElement(std::size_t row);
    Expected<void> removeElement(std::size_t elementRow);
    Expected<void> clearElements(std::size_t row);

    // The component types Add Component offers: every Component type of the session's registry that
    // may be listed (see the header comment) and that not every shown entity has, by category, then
    // label. Empty while the active world is not the one the model last saw (after Play or Stop,
    // before update()), whose handles it would otherwise use.
    [[nodiscard]] std::vector<const TypeInfo*> addableComponents() const;
    // Adds `type` to the shown entities that lack it. Fails with "editor.details.no_entities" and,
    // before update() after Play or Stop, with "editor.details.stale".
    Expected<void> addComponent(const TypeInfo& type);
    Expected<void> removeComponent(const TypeInfo& type);

    // --- Interactions (a drag: one transaction, live preview, one undo entry) ---

    // Opens the interaction on a Property row, in the active world's transactions: a game world's
    // records nothing. Fails with "editor.command.read_only", "editor.command.playing" (the play
    // flag is set but no session runs), "editor.command.interaction_open" (another one is open in
    // either world), "editor.document.busy", "editor.details.no_entities" and "editor.details.stale"
    // (before update() after Play or Stop).
    Expected<void> beginEdit(std::size_t row);

    // Applies `value` (of the edited row's type, clamped) to every shown entity now and to the
    // working copy. Fails with "editor.details.not_editing" without an interaction, and with
    // "editor.details.edit_cancelled" after Esc cancelled it (then nothing changes until it ends).
    Expected<void> previewValue(const void* value);

    // Ends the interaction: one undo entry for everything previewed (none after Esc).
    void commitEdit();

    // Ends the interaction and puts the values back.
    void cancelEdit();

    // True while an interaction of the model is open in the active world; false for one left on a
    // game world that Stop destroyed.
    [[nodiscard]] bool editing() const noexcept;

    // The key of the edited row, or empty (also for an interaction left on a destroyed game world).
    [[nodiscard]] std::string_view editingKey() const noexcept;

    // --- Entity references ---

    // The labelled entities of the world whose label contains `filter` (ASCII case-insensitive), in
    // Outliner order: the choices of a reference field's picker.
    [[nodiscard]] std::vector<EntityChoice> entityChoices(std::string_view filter) const;

    // The label the picker shows for a reference held in the working copy: "None" for the null
    // handle, the entity's label, or its stable id without one; "None" too before update() after
    // Play or Stop, when the handle may be of a destroyed world whose id was recycled.
    [[nodiscard]] std::string entityLabel(Entity entity) const;

    // `id`'s live entity in the active world, as a reference field holds it; the null handle when
    // none.
    [[nodiscard]] Entity entityOf(const Uuid& id) const;

private:
    // The open interaction.
    struct Edit {
        PropertyPath path;
        std::string key;
        // Every access follows endForeignEdit(): once Stop destroyed the game world, the scope's
        // Transactions is gone and only release() may touch it.
        TransactionScope scope;
        // Esc cancelled it (Transactions::cancelOpen); it ends with commitEdit or cancelEdit.
        bool cancelled = false;
        // The identity of the world it edits (ActiveWorld::identity).
        std::uint64_t world = 0;
    };

    // Drops an interaction on a world that is no longer the active one (a game world after Stop,
    // whose transactions are gone) without touching its scope; returns whether it did.
    bool endForeignEdit();
    // The shown entities' live component values differ from the working copy. Allocates nothing.
    [[nodiscard]] bool shownValuesChanged() const;
    // The active world is not the one the rows and handles were built from (Play or Stop since the
    // last update()).
    [[nodiscard]] bool stale() const noexcept;
    void refresh();
    void buildRows();
    void addValueRows(const PropertyPath& path, const FieldInfo* field, std::string label, std::uint32_t depth);
    // The label and path text of each child row of a value that is not inline: a struct's fields, an
    // optional struct's fields when present and not mixed, the elements of arrays of one size.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> childRows(const PropertyPath& path,
                                                                             const void* primary, bool mixed) const;
    [[nodiscard]] bool isExpanded(const std::string& key, bool byDefault) const;
    [[nodiscard]] std::size_t componentIndex(const TypeInfo& type) const;
    [[nodiscard]] const void* valueIn(std::size_t entity, const PropertyPath& path) const;
    // The row when it is a Property row of a non-empty selection, else null.
    [[nodiscard]] const DetailsRow* propertyRow(std::size_t row) const;
    // ids, path and the extra members, run as `command`.
    Expected<void> runOnPath(std::string_view command, const PropertyPath& path, serialization::Value args);

    EditorSession* m_session;
    std::vector<Uuid> m_entities;
    std::vector<Entity> m_handles;
    std::vector<const TypeInfo*> m_components;
    // m_copies[entity][component]: the working copy.
    std::vector<std::vector<OwnedValue>> m_copies;
    std::vector<DetailsRow> m_rows;
    std::string m_label;
    bool m_labelMixed = false;
    std::optional<bool> m_enabled;
    // Keys whose expansion differs from the default.
    std::set<std::string, std::less<>> m_toggled;
    std::optional<Edit> m_edit;
    std::uint64_t m_seenWorld = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t m_seenStructure = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t m_seenDocument = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t m_seenSelection = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t m_seenValues = 0;
    std::uint64_t m_refreshes = 0;
};

// Rotation in the Details panel: X, Y and Z in degrees, pitch about X, yaw about Y and roll about Z
// (Y up, ADR-0006), applied as yaw, then pitch, then roll: q = yaw * pitch * roll. Away from a pitch
// of +-90 degrees eulerDegrees(quatFromEulerDegrees(e)) gives e back, within float precision, for
// pitch in (-90, 90) and yaw and roll in (-180, 180]; at +-90 the roll folds into the yaw.
[[nodiscard]] Vec3 eulerDegrees(const Quat& rotation) noexcept;
[[nodiscard]] Quat quatFromEulerDegrees(const Vec3& degrees) noexcept;

// Clamps a number, or each number of a math vector, at `value` of `type` to the field's Min and Max;
// other values and fields without a range are left alone.
void clampToField(const TypeInfo& type, void* value, const FieldInfo* field) noexcept;

// The name Details and Add Component show for a component type: its DisplayName attribute, else
// its unqualified name spelled as words ("ith::LocalTransform" is "Local Transform").
[[nodiscard]] std::string componentDisplayName(const TypeInfo& type);

// The field or type name spelled as words: "despawnWithParent" is "Despawn With Parent",
// "LocalTransform" is "Local Transform".
[[nodiscard]] std::string displayNameOf(std::string_view name);

} // namespace ith
