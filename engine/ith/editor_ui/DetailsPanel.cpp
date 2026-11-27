#include "ith/editor_ui/DetailsPanel.h"

#include "ith/assets/AssetRef.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_ui/Icons.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/StringUtils.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/TypeOf.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>
#include <vector>

namespace ith {

// What a value editor did this frame. A drag field reports activated, changed and deactivated
// (an interaction); a check box, combo box, picker or text field reports `set` once with its new
// value (one command).
struct DetailsPanel::EditEvents {
    bool activated = false;
    bool changed = false;
    bool deactivated = false;
    bool set = false;

    void merge(const EditEvents& other) {
        activated = activated || other.activated;
        changed = changed || other.changed;
        deactivated = deactivated || other.deactivated;
        set = set || other.set;
    }
};

namespace {

ITH_LOG_CATEGORY(Details);

// Logs a change the panel could not make; Esc ending a drag is not one.
void report(const Expected<void>& result) {
    if (!result && result.error().code != "editor.details.edit_cancelled") {
        ITH_LOG_WARNING(Details, "{}", result.error().message);
    }
}

constexpr const char* kMultipleValues = "Multiple Values";

UiRect rectOfItem() {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return UiRect{.x = min.x, .y = min.y, .width = max.x - min.x, .height = max.y - min.y};
}

// ImGui's data type of a number type; -1 for another kind.
ImGuiDataType dataTypeOf(const TypeInfo& type) {
    switch (type.kind) {
        case TypeKind::Float:
            return type.size == sizeof(float) ? ImGuiDataType_Float : ImGuiDataType_Double;
        case TypeKind::SignedInteger:
            switch (type.size) {
                case 1:
                    return ImGuiDataType_S8;
                case 2:
                    return ImGuiDataType_S16;
                case 4:
                    return ImGuiDataType_S32;
                default:
                    return ImGuiDataType_S64;
            }
        case TypeKind::UnsignedInteger:
            switch (type.size) {
                case 1:
                    return ImGuiDataType_U8;
                case 2:
                    return ImGuiDataType_U16;
                case 4:
                    return ImGuiDataType_U32;
                default:
                    return ImGuiDataType_U64;
            }
        default:
            return -1;
    }
}

// A number of ImGui's data type `dataType` with the value `value`.
std::array<std::byte, 8> numberOf(ImGuiDataType dataType, double value) {
    std::array<std::byte, 8> bytes{};
    const auto store = [&bytes](auto number) { std::memcpy(bytes.data(), &number, sizeof(number)); };
    switch (dataType) {
        case ImGuiDataType_Float:
            store(static_cast<float>(value));
            break;
        case ImGuiDataType_Double:
            store(value);
            break;
        case ImGuiDataType_S8:
            store(static_cast<std::int8_t>(value));
            break;
        case ImGuiDataType_S16:
            store(static_cast<std::int16_t>(value));
            break;
        case ImGuiDataType_S32:
            store(static_cast<std::int32_t>(value));
            break;
        case ImGuiDataType_S64:
            store(static_cast<std::int64_t>(value));
            break;
        case ImGuiDataType_U8:
            store(static_cast<std::uint8_t>(value));
            break;
        case ImGuiDataType_U16:
            store(static_cast<std::uint16_t>(value));
            break;
        case ImGuiDataType_U32:
            store(static_cast<std::uint32_t>(value));
            break;
        default:
            store(static_cast<std::uint64_t>(value));
            break;
    }
    return bytes;
}

// A drag field over `components` numbers of `element` at `value`, limited to the field's Min and Max
// when it has both.
bool dragNumbers(const TypeInfo& element, void* value, int components, const FieldInfo* field, bool mixed) {
    const ImGuiDataType dataType = dataTypeOf(element);
    const bool ranged = field != nullptr && field->attributes.min.has_value() && field->attributes.max.has_value();
    const std::array<std::byte, 8> low = numberOf(dataType, ranged ? *field->attributes.min : 0.0);
    const std::array<std::byte, 8> high = numberOf(dataType, ranged ? *field->attributes.max : 0.0);
    float speed = element.kind == TypeKind::Float ? 0.05f : 0.2f;
    if (ranged && element.kind == TypeKind::Float) {
        speed = static_cast<float>((*field->attributes.max - *field->attributes.min) * 0.005);
    }
    return ImGui::DragScalarN("##value", dataType, value, components, speed, ranged ? low.data() : nullptr,
                              ranged ? high.data() : nullptr, mixed ? kMultipleValues : nullptr);
}

bool isVectorType(const TypeInfo& type) {
    const std::array<std::uint64_t, 10> vectors = {
        typeOf<Vec2>().id,  typeOf<Vec3>().id,  typeOf<Vec4>().id,  typeOf<IVec2>().id, typeOf<IVec3>().id,
        typeOf<IVec4>().id, typeOf<UVec2>().id, typeOf<UVec3>().id, typeOf<UVec4>().id, typeOf<DVec3>().id};
    return std::ranges::find(vectors, type.id) != vectors.end();
}

bool isElementOfVector(const DetailsRow& row) {
    return !row.path.steps().empty() && row.path.steps().back().kind == PathStep::Kind::Index &&
           row.path.withoutLastStep().leafType().kind == TypeKind::DynamicArray;
}

} // namespace

DetailsPanel::DetailsPanel(EditorSession& session) : m_model(session) {}

std::optional<UiRect> DetailsPanel::valueRect(std::string_view key) const {
    const auto found = m_values.find(std::string(key));
    return found != m_values.end() ? std::optional<UiRect>(found->second) : std::nullopt;
}

void DetailsPanel::draw() {
    m_model.update();
    m_values.clear();
    if (m_model.entities().empty()) {
        ImGui::TextDisabled("Select an entity to see its details.");
        return;
    }
    drawHeader();
    ImGui::Separator();
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##properties", 2, kTable)) {
        return;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    // Rows are drawn by index; a command may rebuild them, which this frame tolerates (ids are keys).
    for (std::size_t row = 0; row < m_model.rows().size(); ++row) {
        drawRow(row);
    }
    ImGui::EndTable();
}

void DetailsPanel::drawHeader() {
    // The name, editable with one entity selected.
    ImGui::BeginDisabled(!m_model.canRename());
    constexpr std::string_view kNameKey = "#name";
    const bool owned = m_textKey == kNameKey;
    std::array<char, 1024> local{};
    if (!m_model.labelMixed()) {
        m_model.label().copy(local.data(), local.size() - 1);
    }
    char* text = owned ? m_text.data() : local.data();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputTextWithHint("##name", m_model.labelMixed() ? kMultipleValues : "", text, local.size());
    if (ImGui::IsItemActivated()) {
        m_textKey = kNameKey;
        m_text = local;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && owned) {
        report(m_model.rename(m_text.data()));
    }
    if (ImGui::IsItemDeactivated()) {
        m_textKey.clear();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const std::optional<bool> enabled = m_model.enabled();
    bool on = enabled.value_or(false);
    if (ImGui::Checkbox("Enabled", &on)) {
        report(m_model.setEnabled(on));
    }
    ImGui::SetItemTooltip("%s", enabled.has_value()
                                    ? "A disabled entity and everything below it are skipped by the game"
                                    : kMultipleValues);
    ImGui::SameLine();
    drawAddComponent();
}

void DetailsPanel::drawAddComponent() {
    const std::string label = iconText(icons::kPlus) + "  Add";
    if (ImGui::Button(label.c_str())) {
        m_componentSearch.clear();
        ImGui::OpenPopup("##add_component");
    }
    ImGui::SetItemTooltip("Add Component");
    if (!ImGui::BeginPopup("##add_component")) {
        return;
    }
    std::array<char, 128> search{};
    m_componentSearch.copy(search.data(), search.size() - 1);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::InputTextWithHint("##search", "Search", search.data(), search.size())) {
        m_componentSearch = search.data();
    }
    const std::string needle = toLowerAscii(m_componentSearch);
    std::string_view category = "\x01";
    for (const TypeInfo* type : m_model.addableComponents()) {
        const std::string name = componentDisplayName(*type);
        const std::string lower = toLowerAscii(name);
        if (!needle.empty() && lower.find(needle) == std::string::npos) {
            continue;
        }
        if (type->attributes.category != category) {
            category = type->attributes.category;
            ImGui::SeparatorText(category.empty() ? "Components" : std::string(category).c_str());
        }
        if (ImGui::Selectable((name + "###" + std::string(type->name)).c_str())) {
            report(m_model.addComponent(*type));
            ImGui::CloseCurrentPopup();
        }
        if (!type->attributes.tooltip.empty()) {
            ImGui::SetItemTooltip("%.*s", static_cast<int>(type->attributes.tooltip.size()),
                                  type->attributes.tooltip.data());
        }
    }
    ImGui::EndPopup();
}

void DetailsPanel::drawRow(std::size_t index) {
    // A copy: a command below may rebuild the rows.
    const DetailsRow row = m_model.rows()[index];
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(row.key.c_str());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);
    std::optional<bool> toggled;
    if (row.kind == DetailsRowKind::Component || row.kind == DetailsRowKind::Category || row.expandable) {
        // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (row.kind == DetailsRowKind::Component) {
            flags |= ImGuiTreeNodeFlags_Framed;
        }
        if (!row.expandable) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        // NOLINTEND(bugprone-signed-bitwise)
        if (row.expandable) {
            ImGui::SetNextItemOpen(row.expanded);
        }
        const bool open = ImGui::TreeNodeEx("##node", flags, "%s", row.label.c_str());
        if (ImGui::IsItemToggledOpen()) {
            toggled = open;
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(row.label.c_str());
    }
    if (!row.tooltip.empty()) {
        ImGui::SetItemTooltip("%s", row.tooltip.c_str());
    }

    ImGui::TableSetColumnIndex(1);
    if (row.kind == DetailsRowKind::Component) {
        const std::string remove = iconText(icons::kTrash) + "##remove";
        if (ImGui::SmallButton(remove.c_str())) {
            report(m_model.removeComponent(*row.type));
        }
        ImGui::SetItemTooltip("Remove Component");
    } else if (row.kind == DetailsRowKind::Property) {
        drawValue(index);
    }
    ImGui::PopID();
    if (toggled.has_value()) {
        m_model.setExpanded(index, *toggled);
    }
}

void DetailsPanel::applyEdit(std::size_t index, const std::string& key, EditEvents events, const void* edited) {
    if (events.changed && !events.activated && !m_model.editing() && m_refusedKey != key) {
        // The press opens a drag edit and its release ends it, empty; ImGui then opens the text
        // input with no new activation, so the typed value arrives as a change with no edit open
        // and is set with one command.
        events.set = true;
    }
    if (events.activated && !m_model.editing()) {
        const Expected<void> begun = m_model.beginEdit(index);
        if (!begun) {
            m_refusedKey = key;
        }
        report(begun);
    }
    if (events.deactivated && m_refusedKey == key) {
        m_refusedKey.clear();
    }
    const bool ours = m_model.editing() && m_model.editingKey() == key;
    if (events.changed && ours) {
        report(m_model.previewValue(edited));
    }
    if (events.set) {
        report(m_model.setValue(index, edited));
    }
    if (events.deactivated && ours) {
        m_model.commitEdit();
    }
}

void DetailsPanel::drawValue(std::size_t index) {
    const DetailsRow row = m_model.rows()[index];
    const void* current = m_model.value(index);
    ImGui::BeginDisabled(row.readOnly);
    const float reset =
        row.isDefault || row.readOnly ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float remove = isElementOfVector(row) ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
    if (row.inlineValue && current != nullptr) {
        OwnedValue edited = OwnedValue::copyOf(*row.type, current);
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - reset - remove, 1.0f));
        const EditEvents events = drawEditor(row, *row.type, edited.get(), row.mixed);
        m_values[row.key] = rectOfItem();
        applyEdit(index, row.key, events, edited.get());
    } else if (row.type->kind == TypeKind::DynamicArray) {
        if (row.mixed) {
            ImGui::TextDisabled("%s", kMultipleValues);
        } else if (current != nullptr) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%zu elements", row.type->container->size(current));
        }
        ImGui::SameLine();
        const std::string add = iconText(icons::kPlus) + "##add";
        if (ImGui::SmallButton(add.c_str())) {
            report(m_model.addElement(index));
        }
        ImGui::SetItemTooltip("Add Element");
        ImGui::SameLine();
        const std::string clear = iconText(icons::kTrash) + "##clear";
        if (ImGui::SmallButton(clear.c_str())) {
            report(m_model.clearElements(index));
        }
        ImGui::SetItemTooltip("Remove All Elements");
    } else if (row.type->kind == TypeKind::Optional && current != nullptr) {
        bool present = row.type->container->size(current) == 1;
        if (ImGui::Checkbox("##present", &present)) {
            report(m_model.setPresent(index, present));
        }
        m_values[row.key] = rectOfItem();
    } else if (row.type->kind == TypeKind::FixedArray && current != nullptr) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%zu elements", row.type->container->size(current));
    }
    if (isElementOfVector(row)) {
        ImGui::SameLine();
        const std::string erase = iconText(icons::kClose) + "##erase";
        if (ImGui::SmallButton(erase.c_str())) {
            report(m_model.removeElement(index));
        }
        ImGui::SetItemTooltip("Remove Element");
    }
    if (reset > 0.0f) {
        ImGui::SameLine();
        const std::string label = iconText(icons::kReset) + "##reset";
        if (ImGui::SmallButton(label.c_str())) {
            report(m_model.resetToDefault(index));
        }
        ImGui::SetItemTooltip("Reset to Default");
    }
    ImGui::EndDisabled();
}

namespace {

// The events of the drag field just drawn.
DetailsPanel::EditEvents dragEvents(bool changed) {
    return DetailsPanel::EditEvents{
        .activated = ImGui::IsItemActivated(), .changed = changed, .deactivated = ImGui::IsItemDeactivated()};
}

void drawAssetRef(const TypeInfo& type, const void* value, bool mixed) {
    // An id display until the Content Browser can pick assets.
    const Uuid& asset = type.id == typeOf<AssetRef>().id ? static_cast<const AssetRef*>(value)->asset
                                                         : static_cast<const SoftAssetRef*>(value)->asset;
    std::string text = asset.isNil() ? std::string("None") : asset.toString();
    if (mixed) {
        text = kMultipleValues;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text.c_str());
}

DetailsPanel::EditEvents drawEnum(const TypeInfo& type, void* value, bool mixed) {
    const std::int64_t current = readEnumValue(type, value);
    const auto found = std::ranges::find_if(
        type.enumerators, [current](const EnumeratorInfo& enumerator) { return enumerator.value == current; });
    std::string preview = found != type.enumerators.end() ? displayNameOf(found->name) : std::to_string(current);
    if (mixed) {
        preview = kMultipleValues;
    }
    DetailsPanel::EditEvents events;
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        for (const EnumeratorInfo& enumerator : type.enumerators) {
            const std::string name = displayNameOf(enumerator.name);
            if (ImGui::Selectable(name.c_str(), !mixed && enumerator.value == current)) {
                writeEnumValue(type, value, enumerator.value);
                events.set = true;
            }
        }
        ImGui::EndCombo();
    }
    return events;
}

DetailsPanel::EditEvents drawBool(bool& value, bool mixed) {
    bool on = value && !mixed;
    const bool changed = ImGui::Checkbox("##value", &on);
    if (mixed) {
        ImGui::SetItemTooltip("%s", kMultipleValues);
    }
    value = on;
    return DetailsPanel::EditEvents{.set = changed};
}

} // namespace

DetailsPanel::EditEvents DetailsPanel::drawRotation(const DetailsRow& row, Quat& value, bool mixed) {
    // Kept as degrees while it is dragged, so it doesn't drift or flip.
    Vec3 euler = m_eulerKey == row.key ? m_euler : eulerDegrees(value);
    const bool changed = ImGui::DragFloat3("##value", &euler.x, 0.5f, 0.0f, 0.0f, mixed ? kMultipleValues : "%.2f");
    if (ImGui::IsItemActive()) {
        m_eulerKey = row.key;
        m_euler = euler;
    }
    if (changed) {
        value = quatFromEulerDegrees(euler);
    }
    const EditEvents events = dragEvents(changed);
    if (events.deactivated) {
        m_eulerKey.clear();
    }
    return events;
}

DetailsPanel::EditEvents DetailsPanel::drawEditor(const DetailsRow& row, const TypeInfo& type, void* value,
                                                  bool mixed) {
    // By type id first, then by kind.
    if (type.id == typeOf<Quat>().id) {
        return drawRotation(row, *static_cast<Quat*>(value), mixed);
    }
    if (isVectorType(type)) {
        return dragEvents(
            dragNumbers(type.fields.front().type(), value, static_cast<int>(type.fields.size()), row.field, mixed));
    }
    if (type.id == typeOf<AssetRef>().id || type.id == typeOf<SoftAssetRef>().id) {
        drawAssetRef(type, value, mixed);
        return {};
    }
    switch (type.kind) {
        case TypeKind::Bool:
            return drawBool(*static_cast<bool*>(value), mixed);
        case TypeKind::SignedInteger:
        case TypeKind::UnsignedInteger:
        case TypeKind::Float:
            return dragEvents(dragNumbers(type, value, 1, row.field, mixed));
        case TypeKind::String:
            return drawText(row, *static_cast<std::string*>(value), mixed);
        case TypeKind::Uuid:
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", static_cast<const Uuid*>(value)->toString().c_str());
            return {};
        case TypeKind::Enum:
            return drawEnum(type, value, mixed);
        case TypeKind::Entity:
            return drawEntity(row, *static_cast<Entity*>(value), mixed);
        case TypeKind::Optional:
            return drawOptional(row, type, value, mixed);
        default:
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("(no editor)");
            return {};
    }
}

DetailsPanel::EditEvents DetailsPanel::drawOptional(const DetailsRow& row, const TypeInfo& type, void* value,
                                                    bool mixed) {
    bool present = type.container->size(value) == 1;
    EditEvents events;
    if (ImGui::Checkbox("##present", &present)) {
        type.container->resize(value, present ? 1 : 0);
        events.set = true;
    }
    if (present && !events.set) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, 1.0f));
        events.merge(drawEditor(row, type.element(), type.container->at(value, 0), mixed));
    }
    return events;
}

DetailsPanel::EditEvents DetailsPanel::drawText(const DetailsRow& row, std::string& value, bool mixed) {
    const bool owned = m_textKey == row.key;
    std::array<char, 1024> local{};
    if (!mixed) {
        value.copy(local.data(), local.size() - 1);
    }
    char* text = owned ? m_text.data() : local.data();
    ImGui::InputTextWithHint("##value", mixed ? kMultipleValues : "", text, local.size());
    if (ImGui::IsItemActivated()) {
        m_textKey = row.key;
        m_text = local;
    }
    EditEvents events;
    if (ImGui::IsItemDeactivatedAfterEdit() && owned) {
        value = m_text.data();
        events.set = true;
    }
    if (ImGui::IsItemDeactivated()) {
        m_textKey.clear();
    }
    return events;
}

DetailsPanel::EditEvents DetailsPanel::drawEntity(const DetailsRow& /*row*/, Entity& value, bool mixed) {
    const std::string preview = mixed ? std::string(kMultipleValues) : m_model.entityLabel(value);
    EditEvents events;
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    if (!ImGui::BeginCombo("##value", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        return events;
    }
    std::array<char, 128> search{};
    if (ImGui::IsWindowAppearing()) {
        m_entitySearch.clear();
        ImGui::SetKeyboardFocusHere();
    }
    m_entitySearch.copy(search.data(), search.size() - 1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##search", "Search by name", search.data(), search.size())) {
        m_entitySearch = search.data();
    }
    if (ImGui::Selectable("None", !mixed && value.isNull())) {
        value = Entity();
        events.set = true;
    }
    for (const EntityChoice& choice : m_model.entityChoices(m_entitySearch)) {
        const std::string label = choice.label + "###" + choice.id.toString();
        if (ImGui::Selectable(label.c_str(), !mixed && m_model.entityOf(choice.id) == value)) {
            value = m_model.entityOf(choice.id);
            events.set = true;
        }
        ImGui::SetItemTooltip("%s", choice.id.toString().c_str());
    }
    ImGui::EndCombo();
    return events;
}

void registerDetails(PanelRegistry& panels, DetailsPanel& panel) {
    panels.add({.id = "ith.details",
                .displayName = "Details",
                .defaultDock = DockPlace::Right,
                .draw = [&panel](std::string_view /*instance*/) { panel.draw(); }});
}

} // namespace ith
