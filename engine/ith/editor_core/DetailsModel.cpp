#include "ith/editor_core/DetailsModel.h"

#include "ith/assets/AssetRef.h"
#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/foundation/StringUtils.h"
#include "ith/math/Math.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/TextArchive.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace ith {

namespace {

using serialization::Value;

// The component types the World, the Outliner or the header keep: never listed as groups.
bool keptElsewhere(const TypeInfo& type) {
    const std::array<std::uint64_t, 5> kept = {typeOf<StableId>().id, typeOf<Children>().id,
                                               typeOf<DisabledInHierarchy>().id, typeOf<EditorLabel>().id,
                                               typeOf<Disabled>().id};
    return std::ranges::find(kept, type.id) != kept.end();
}

bool listed(const TypeInfo& type) {
    return type.role == TypeRole::Component && !hasFlags(type.flags, ReflectionFlags::Transient) &&
           !keptElsewhere(type);
}

// The types one inline editor shows whole, beyond the leaf kinds.
bool inlineStruct(const TypeInfo& type) {
    const std::array<std::uint64_t, 13> types = {
        typeOf<Vec2>().id,  typeOf<Vec3>().id,     typeOf<Vec4>().id,        typeOf<IVec2>().id, typeOf<IVec3>().id,
        typeOf<IVec4>().id, typeOf<UVec2>().id,    typeOf<UVec3>().id,       typeOf<UVec4>().id, typeOf<DVec3>().id,
        typeOf<Quat>().id,  typeOf<AssetRef>().id, typeOf<SoftAssetRef>().id};
    return std::ranges::find(types, type.id) != types.end();
}

bool hasEditorMeta(const FieldInfo* field) {
    return field != nullptr &&
           std::ranges::any_of(field->attributes.meta, [](const MetaEntry& entry) { return entry.key == "Editor"; });
}

bool isInline(const TypeInfo& type, const FieldInfo* field) {
    if (hasEditorMeta(field)) {
        return true;
    }
    switch (type.kind) {
        case TypeKind::Struct:
            return inlineStruct(type);
        case TypeKind::FixedArray:
        case TypeKind::DynamicArray:
            return false;
        case TypeKind::Optional:
            return isInline(type.element(), nullptr);
        default:
            return true;
    }
}

std::string shortName(std::string_view name) {
    const std::size_t at = name.rfind("::");
    return std::string(at == std::string_view::npos ? name : name.substr(at + 2));
}

std::string componentLabel(const TypeInfo& type) {
    return type.attributes.displayName.empty() ? displayNameOf(shortName(type.name))
                                               : std::string(type.attributes.displayName);
}

std::string fieldLabel(const FieldInfo& field) {
    return field.attributes.displayName.empty() ? displayNameOf(field.name) : std::string(field.attributes.displayName);
}

std::string fieldTooltip(const FieldInfo* field) {
    if (field == nullptr) {
        return {};
    }
    return std::string(field->attributes.tooltip.empty() ? field->name : field->attributes.tooltip);
}

Value idsValue(std::span<const Uuid> ids) {
    Value list = Value::array();
    for (const Uuid& id : ids) {
        list.elements().push_back(Value::string(id.toString()));
    }
    return list;
}

Expected<void> run(EditorSession& session, std::string_view command, const Value& args) {
    Expected<Value> result = session.execute(command, args);
    if (!result) {
        return makeUnexpected(std::move(result).error());
    }
    return {};
}

template <typename T>
void clampAs(void* value, double low, double high) {
    T& number = *static_cast<T*>(value);
    const auto wide = static_cast<double>(number);
    if (wide < low) {
        number = static_cast<T>(low);
    } else if (wide > high) {
        number = static_cast<T>(high);
    }
}

void clampNumber(const TypeInfo& type, void* value, double low, double high) noexcept {
    switch (type.kind) {
        case TypeKind::Float:
            if (type.size == sizeof(float)) {
                clampAs<float>(value, low, high);
            } else {
                clampAs<double>(value, low, high);
            }
            break;
        case TypeKind::SignedInteger:
            switch (type.size) {
                case 1:
                    clampAs<std::int8_t>(value, low, high);
                    break;
                case 2:
                    clampAs<std::int16_t>(value, low, high);
                    break;
                case 4:
                    clampAs<std::int32_t>(value, low, high);
                    break;
                default:
                    clampAs<std::int64_t>(value, low, high);
                    break;
            }
            break;
        case TypeKind::UnsignedInteger:
            switch (type.size) {
                case 1:
                    clampAs<std::uint8_t>(value, low, high);
                    break;
                case 2:
                    clampAs<std::uint16_t>(value, low, high);
                    break;
                case 4:
                    clampAs<std::uint32_t>(value, low, high);
                    break;
                default:
                    clampAs<std::uint64_t>(value, low, high);
                    break;
            }
            break;
        default:
            break;
    }
}

} // namespace

// --- Free functions ---

Vec3 eulerDegrees(const Quat& rotation) noexcept {
    // m[c][r] is row r, column c of R = Ry(yaw) * Rx(pitch) * Rz(roll).
    const Mat3 m = glm::mat3_cast(glm::normalize(rotation));
    const float sinPitch = std::clamp(-m[2][1], -1.0f, 1.0f);
    const float pitch = std::asin(sinPitch);
    float yaw = 0.0f;
    float roll = 0.0f;
    if (std::abs(sinPitch) < 0.9999f) {
        yaw = std::atan2(m[2][0], m[2][2]);
        roll = std::atan2(m[0][1], m[1][1]);
    } else {
        // Gimbal lock: the roll folds into the yaw.
        yaw = std::atan2(-m[0][2], m[0][0]);
    }
    return glm::degrees(Vec3(pitch, yaw, roll));
}

Quat quatFromEulerDegrees(const Vec3& degrees) noexcept {
    const Vec3 radians = glm::radians(degrees);
    return glm::angleAxis(radians.y, Vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(radians.x, Vec3(1.0f, 0.0f, 0.0f)) *
           glm::angleAxis(radians.z, Vec3(0.0f, 0.0f, 1.0f));
}

std::string componentDisplayName(const TypeInfo& type) {
    return componentLabel(type);
}

void clampToField(const TypeInfo& type, void* value, const FieldInfo* field) noexcept {
    if (field == nullptr || (!field->attributes.min.has_value() && !field->attributes.max.has_value())) {
        return;
    }
    const double low = field->attributes.min.value_or(-std::numeric_limits<double>::infinity());
    const double high = field->attributes.max.value_or(std::numeric_limits<double>::infinity());
    if (type.kind == TypeKind::Struct && inlineStruct(type)) {
        for (const FieldInfo& part : type.fields) {
            clampNumber(part.type(), static_cast<std::byte*>(value) + part.offset, low, high);
        }
        return;
    }
    clampNumber(type, value, low, high);
}

std::string displayNameOf(std::string_view name) {
    std::string words;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool upper = c >= 'A' && c <= 'Z';
        if (i > 0 && upper) {
            const char previous = name[i - 1];
            const bool afterLower = (previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9');
            const bool beforeLower =
                i + 1 < name.size() && name[i + 1] >= 'a' && name[i + 1] <= 'z' && previous >= 'A' && previous <= 'Z';
            if (afterLower || beforeLower) {
                words += ' ';
            }
        }
        words += (i == 0 && c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return words;
}

// --- DetailsModel ---

DetailsModel::DetailsModel(EditorSession& session) : m_session(&session) {}

DetailsModel::~DetailsModel() {
    if (!endForeignEdit() && m_edit.has_value()) {
        m_edit->scope.cancel();
    }
}

bool DetailsModel::endForeignEdit() {
    if (!m_edit.has_value() || m_edit->world == m_session->active().identity) {
        return false;
    }
    // Stop destroyed the game world's transactions after cancelling the edit: forget the scope.
    m_edit->scope.release();
    m_edit.reset();
    return true;
}

void DetailsModel::update() {
    const ActiveWorld active = m_session->active();
    const EditorChanges& changes = active.changes;
    bool needed = endForeignEdit();
    if (m_edit.has_value() && !m_edit->cancelled && !m_edit->scope.active()) {
        // Esc cancelled the interaction: the world has its old values back.
        m_edit->cancelled = true;
        needed = true;
    }
    if (m_edit.has_value() && !m_edit->cancelled) {
        return; // our own interaction is open: its previews are the working copy
    }
    // Another world (Play, Stop) always rebuilds, whatever its counters say (ADR-0053 decision 12).
    if (active.identity != m_seenWorld || changes.structure() != m_seenStructure ||
        changes.document() != m_seenDocument || changes.selection() != m_seenSelection) {
        needed = true;
    }
    if (!needed && changes.values() != m_seenValues) {
        const std::optional<std::vector<Uuid>> touched = changes.touchedSince(m_seenValues);
        if (touched.has_value()) {
            needed = std::ranges::any_of(*touched, [this](const Uuid& id) {
                return std::ranges::any_of(m_entities, [&id](const Uuid& shown) { return shown == id; });
            });
        } else {
            // No log (a game world's tick says "everything may have changed"): compare the shown
            // values with the working copy, so a tick that leaves them alone costs no refresh and
            // no allocation (spec requirement 47).
            needed = shownValuesChanged();
        }
    }
    m_seenWorld = active.identity;
    m_seenStructure = changes.structure();
    m_seenDocument = changes.document();
    m_seenSelection = changes.selection();
    m_seenValues = changes.values();
    if (needed) {
        refresh();
    }
}

bool DetailsModel::shownValuesChanged() const {
    const World& world = m_session->active().world;
    for (std::size_t entity = 0; entity < m_handles.size() && entity < m_copies.size(); ++entity) {
        for (std::size_t component = 0; component < m_components.size(); ++component) {
            const TypeInfo& type = *m_components[component];
            const void* live = world.get(m_handles[entity], type);
            if (live == nullptr || type.equals == nullptr || !type.equals(live, m_copies[entity][component].get())) {
                return true;
            }
        }
    }
    return false;
}

bool DetailsModel::stale() const noexcept {
    return m_seenWorld != m_session->active().identity;
}

void DetailsModel::refresh() {
    ++m_refreshes;
    const World& world = m_session->active().world;
    m_entities.clear();
    m_handles.clear();
    for (const Uuid& id : m_session->active().selection.entities()) {
        const Entity entity = world.findByStableId(id);
        if (!entity.isNull()) {
            m_entities.push_back(id);
            m_handles.push_back(entity);
        }
    }

    m_label.clear();
    m_labelMixed = false;
    m_enabled.reset();
    m_components.clear();
    m_copies.clear();
    if (!m_handles.empty()) {
        const auto labelOf = [&](std::size_t i) {
            const std::string* label = detail::labelOf(*m_session, world, m_handles[i]);
            return label != nullptr ? *label : m_entities[i].toString();
        };
        m_label = labelOf(m_handles.size() - 1);
        std::size_t disabled = 0;
        for (std::size_t i = 0; i < m_handles.size(); ++i) {
            m_labelMixed = m_labelMixed || labelOf(i) != m_label;
            disabled += world.has<Disabled>(m_handles[i]) ? 1 : 0;
        }
        if (disabled == 0 || disabled == m_handles.size()) {
            m_enabled = disabled == 0;
        }
        world.forEachComponent(m_handles.back(), [&](const TypeInfo& type, const void*) {
            const bool everywhere =
                std::ranges::all_of(m_handles, [&](Entity entity) { return world.has(entity, type); });
            if (listed(type) && everywhere) {
                m_components.push_back(&type);
            }
        });
        const std::uint64_t transform = typeOf<LocalTransform>().id;
        std::ranges::sort(m_components, [transform](const TypeInfo* a, const TypeInfo* b) {
            if ((a->id == transform) != (b->id == transform)) {
                return a->id == transform;
            }
            return componentLabel(*a) < componentLabel(*b);
        });
        for (const Entity entity : m_handles) {
            std::vector<OwnedValue>& copies = m_copies.emplace_back();
            for (const TypeInfo* type : m_components) {
                copies.push_back(OwnedValue::copyOf(*type, world.get(entity, *type)));
            }
        }
    }
    buildRows();
}

bool DetailsModel::isExpanded(const std::string& key, bool byDefault) const {
    return m_toggled.contains(key) != byDefault;
}

std::size_t DetailsModel::componentIndex(const TypeInfo& type) const {
    const auto found = std::ranges::find_if(m_components, [&](const TypeInfo* shown) { return shown->id == type.id; });
    return static_cast<std::size_t>(found - m_components.begin());
}

const void* DetailsModel::valueIn(std::size_t entity, const PropertyPath& path) const {
    const std::size_t component = componentIndex(path.rootType());
    if (entity >= m_copies.size() || component >= m_components.size()) {
        return nullptr;
    }
    const Expected<const void*> value = resolve(path, m_copies[entity][component].get());
    return value ? *value : nullptr;
}

void DetailsModel::buildRows() {
    m_rows.clear();
    for (const TypeInfo* type : m_components) {
        const std::string key(type->name);
        const bool expanded = isExpanded(key, true);
        m_rows.push_back(DetailsRow{
            .kind = DetailsRowKind::Component,
            .depth = 0,
            .label = componentLabel(*type),
            .tooltip = std::string(type->attributes.tooltip.empty() ? type->name : type->attributes.tooltip),
            .type = type,
            .component = type,
            .path = PropertyPath(*type),
            .key = key,
            .expandable = !type->fields.empty(),
            .expanded = expanded && !type->fields.empty(),
        });
        if (!expanded) {
            continue;
        }
        // Fields without a category first, then each category's, in the order they first appear.
        std::vector<std::string_view> categories;
        for (const FieldInfo& field : type->fields) {
            if (!field.attributes.category.empty() &&
                std::ranges::find(categories, field.attributes.category) == categories.end()) {
                categories.push_back(field.attributes.category);
            }
        }
        const auto addField = [&](const FieldInfo& field, std::uint32_t depth) {
            Expected<PropertyPath> path = PropertyPath::parse(*type, field.name);
            if (path) {
                addValueRows(*path, &field, fieldLabel(field), depth);
            }
        };
        for (const FieldInfo& field : type->fields) {
            if (field.attributes.category.empty()) {
                addField(field, 1);
            }
        }
        for (const std::string_view category : categories) {
            std::string categoryKey = key + "#" + std::string(category);
            const bool open = isExpanded(categoryKey, true);
            m_rows.push_back(DetailsRow{.kind = DetailsRowKind::Category,
                                        .depth = 1,
                                        .label = std::string(category),
                                        .component = type,
                                        .path = PropertyPath(*type),
                                        .key = std::move(categoryKey),
                                        .expandable = true,
                                        .expanded = open});
            if (open) {
                for (const FieldInfo& field : type->fields) {
                    if (field.attributes.category == category) {
                        addField(field, 2);
                    }
                }
            }
        }
    }
}

std::vector<std::pair<std::string, std::string>> DetailsModel::childRows(const PropertyPath& path, const void* primary,
                                                                         bool mixed) const {
    std::vector<std::pair<std::string, std::string>> children;
    const TypeInfo& type = path.leafType();
    const std::string text = path.toQualifiedString();
    const TypeInfo* structType = nullptr;
    if (type.kind == TypeKind::Struct) {
        structType = &type;
    } else if (type.kind == TypeKind::Optional && type.element().kind == TypeKind::Struct && !mixed &&
               type.container->size(primary) == 1) {
        structType = &type.element();
    }
    if (structType != nullptr) {
        for (const FieldInfo& child : structType->fields) {
            children.emplace_back(fieldLabel(child), text + "." + std::string(child.name));
        }
        return children;
    }
    if (type.kind != TypeKind::FixedArray && type.kind != TypeKind::DynamicArray) {
        return children;
    }
    // Element rows only when every entity's array has the same size.
    const std::size_t size = type.container->size(primary);
    for (std::size_t i = 0; i + 1 < m_copies.size(); ++i) {
        const void* other = valueIn(i, path);
        if (other == nullptr || type.container->size(other) != size) {
            return children;
        }
    }
    for (std::size_t i = 0; i < size; ++i) {
        children.emplace_back("[" + std::to_string(i) + "]", text + "[" + std::to_string(i) + "]");
    }
    return children;
}

void DetailsModel::addValueRows(const PropertyPath& path, const FieldInfo* field, std::string label,
                                std::uint32_t depth) {
    const TypeInfo& type = path.leafType();
    DetailsRow row{.kind = DetailsRowKind::Property,
                   .depth = depth,
                   .label = std::move(label),
                   .tooltip = fieldTooltip(field),
                   .type = &type,
                   .component = &path.rootType(),
                   .field = field,
                   .path = path,
                   .key = path.toQualifiedString(),
                   .readOnly = hasFlags(path.chainFlags(), ReflectionFlags::ReadOnly),
                   .inlineValue = isInline(type, field)};

    // Values across the entities: mixed when any differs or reaches less far than the primary.
    const void* primary = valueIn(m_copies.size() - 1, path);
    for (std::size_t i = 0; i + 1 < m_copies.size(); ++i) {
        const void* other = valueIn(i, path);
        if ((other == nullptr) != (primary == nullptr) || (other != nullptr && !type.equals(other, primary))) {
            row.mixed = true;
            break;
        }
    }
    if (row.mixed || primary == nullptr) {
        row.isDefault = !row.mixed && primary == nullptr;
    } else {
        const OwnedValue fallback = detail::defaultValueAt(path);
        row.isDefault = type.equals(primary, fallback.get());
    }

    std::vector<std::pair<std::string, std::string>> children;
    if (!row.inlineValue && primary != nullptr) {
        children = childRows(path, primary, row.mixed);
    }
    const bool isArray = type.kind == TypeKind::FixedArray || type.kind == TypeKind::DynamicArray;
    row.expandable = !children.empty();
    row.expanded = row.expandable && isExpanded(row.key, !isArray);
    const bool expanded = row.expanded;
    m_rows.push_back(std::move(row));
    if (!expanded) {
        return;
    }
    for (auto& [childLabel, childText] : children) {
        Expected<PropertyPath> child = PropertyPath::parseQualified(m_session->types(), childText);
        if (child) {
            addValueRows(*child, child->leafField(), std::move(childLabel), depth + 1);
        }
    }
}

std::optional<std::size_t> DetailsModel::findRow(std::string_view key) const {
    const auto found = std::ranges::find_if(m_rows, [key](const DetailsRow& row) { return row.key == key; });
    return found != m_rows.end() ? std::optional<std::size_t>(static_cast<std::size_t>(found - m_rows.begin()))
                                 : std::nullopt;
}

const void* DetailsModel::value(std::size_t row) const {
    if (row >= m_rows.size() || m_rows[row].kind == DetailsRowKind::Category || m_copies.empty()) {
        return nullptr;
    }
    return valueIn(m_copies.size() - 1, m_rows[row].path);
}

void DetailsModel::setExpanded(std::size_t row, bool expanded) {
    if (row >= m_rows.size() || !m_rows[row].expandable || m_rows[row].expanded == expanded) {
        return;
    }
    const std::string& key = m_rows[row].key;
    if (m_toggled.erase(key) == 0) {
        m_toggled.insert(key);
    }
    buildRows();
}

const DetailsRow* DetailsModel::propertyRow(std::size_t row) const {
    if (row >= m_rows.size() || m_rows[row].kind != DetailsRowKind::Property || m_entities.empty()) {
        return nullptr;
    }
    return &m_rows[row];
}

Expected<void> DetailsModel::runOnPath(std::string_view command, const PropertyPath& path, Value args) {
    args.set("ids", idsValue(m_entities));
    args.set("path", Value::string(path.toQualifiedString()));
    Expected<void> result = run(*m_session, command, args);
    if (result) {
        refresh();
    }
    return result;
}

// --- Header ---

Expected<void> DetailsModel::rename(std::string_view label) {
    if (!canRename()) {
        return makeError("editor.details.rename_one", "the name can be changed with one entity selected");
    }
    Value args = Value::object();
    args.set("id", Value::string(m_entities.front().toString()));
    args.set("label", Value::string(std::string(label)));
    return run(*m_session, "ith.edit.rename", args);
}

Expected<void> DetailsModel::setEnabled(bool enabled) {
    if (m_entities.empty()) {
        return makeError("editor.details.no_entities", "nothing is selected");
    }
    Value args = Value::object();
    args.set("ids", idsValue(m_entities));
    args.set("enabled", Value::boolean(enabled));
    return run(*m_session, "ith.entity.set_enabled", args);
}

// --- One-shot edits ---

namespace {

Unexpected<Error> noProperty() {
    return makeError("editor.details.no_property", "the row isn't a property of the selection");
}

} // namespace

Expected<void> DetailsModel::setValue(std::size_t row, const void* value) {
    const DetailsRow* property = propertyRow(row);
    if (property == nullptr) {
        return noProperty();
    }
    OwnedValue copy = OwnedValue::copyOf(*property->type, value);
    clampToField(*property->type, copy.get(), property->field);
    auto json = serialization::toValue(*property->type, copy.get(), &m_session->active().entities);
    if (!json) {
        return detail::fromSerialization(json.error());
    }
    Value args = Value::object();
    args.set("value", std::move(*json));
    return runOnPath("ith.entity.set_property", property->path, std::move(args));
}

Expected<void> DetailsModel::resetToDefault(std::size_t row) {
    const DetailsRow* property = propertyRow(row);
    if (property == nullptr) {
        return noProperty();
    }
    return runOnPath("ith.entity.reset_property", property->path, Value::object());
}

Expected<void> DetailsModel::setPresent(std::size_t row, bool present) {
    const DetailsRow* property = propertyRow(row);
    if (property == nullptr || property->type->kind != TypeKind::Optional) {
        return makeError("editor.details.not_optional", "the row isn't an optional value");
    }
    Value args = Value::object();
    if (present) {
        const TypeInfo& element = property->type->element();
        const OwnedValue fresh(element);
        auto json = serialization::toValue(element, fresh.get(), &m_session->active().entities);
        if (!json) {
            return detail::fromSerialization(json.error());
        }
        args.set("value", std::move(*json));
    } else {
        args.set("value", Value());
    }
    return runOnPath("ith.entity.set_property", property->path, std::move(args));
}

Expected<void> DetailsModel::addElement(std::size_t row) {
    const DetailsRow* property = propertyRow(row);
    if (property == nullptr || property->type->kind != TypeKind::DynamicArray) {
        return makeError("editor.details.not_a_list", "the row isn't a list");
    }
    return runOnPath("ith.entity.insert_element", property->path, Value::object());
}

Expected<void> DetailsModel::removeElement(std::size_t elementRow) {
    const DetailsRow* property = propertyRow(elementRow);
    const bool element = property != nullptr && !property->path.steps().empty() &&
                         property->path.steps().back().kind == PathStep::Kind::Index &&
                         property->path.withoutLastStep().leafType().kind == TypeKind::DynamicArray;
    if (!element) {
        return makeError("editor.details.not_an_element", "the row isn't an element of a list");
    }
    Value args = Value::object();
    args.set("index", Value::unsignedInteger(property->path.steps().back().index));
    return runOnPath("ith.entity.erase_element", property->path.withoutLastStep(), std::move(args));
}

Expected<void> DetailsModel::clearElements(std::size_t row) {
    const DetailsRow* property = propertyRow(row);
    if (property == nullptr || property->type->kind != TypeKind::DynamicArray) {
        return makeError("editor.details.not_a_list", "the row isn't a list");
    }
    Value args = Value::object();
    args.set("value", Value::array());
    return runOnPath("ith.entity.set_property", property->path, std::move(args));
}

std::vector<const TypeInfo*> DetailsModel::addableComponents() const {
    std::vector<const TypeInfo*> types;
    if (stale() || m_handles.empty()) {
        return types;
    }
    const World& world = m_session->active().world;
    for (const TypeInfo* type : m_session->types().types()) {
        if (!listed(*type) || type->id == typeOf<ChildOf>().id) {
            continue;
        }
        if (!std::ranges::all_of(m_handles, [&](Entity entity) { return world.has(entity, *type); })) {
            types.push_back(type);
        }
    }
    std::ranges::sort(types, [](const TypeInfo* a, const TypeInfo* b) {
        if (a->attributes.category != b->attributes.category) {
            return a->attributes.category < b->attributes.category;
        }
        return componentLabel(*a) < componentLabel(*b);
    });
    return types;
}

Expected<void> DetailsModel::addComponent(const TypeInfo& type) {
    if (stale()) {
        return makeError("editor.details.stale", "the shown entities are of another world; update first");
    }
    if (m_entities.empty()) {
        return makeError("editor.details.no_entities", "nothing is selected");
    }
    // Only to the entities that don't have it: the others keep their values.
    const World& world = m_session->active().world;
    std::vector<Uuid> missing;
    for (std::size_t i = 0; i < m_handles.size(); ++i) {
        if (!world.has(m_handles[i], type)) {
            missing.push_back(m_entities[i]);
        }
    }
    if (missing.empty()) {
        return {};
    }
    Value args = Value::object();
    args.set("ids", idsValue(missing));
    args.set("type", Value::string(std::string(type.name)));
    return run(*m_session, "ith.entity.add_component", args);
}

Expected<void> DetailsModel::removeComponent(const TypeInfo& type) {
    if (m_entities.empty()) {
        return makeError("editor.details.no_entities", "nothing is selected");
    }
    Value args = Value::object();
    args.set("ids", idsValue(m_entities));
    args.set("type", Value::string(std::string(type.name)));
    return run(*m_session, "ith.entity.remove_component", args);
}

// --- Interactions ---

Expected<void> DetailsModel::beginEdit(std::size_t row) {
    endForeignEdit();
    if (stale()) {
        return makeError("editor.details.stale", "the shown entities are of another world; update first");
    }
    EditorDocument& document = m_session->document();
    const ActiveWorld active = m_session->active();
    if (m_edit.has_value() || document.transactions().isOpen() || active.transactions.isOpen()) {
        return makeError("editor.command.interaction_open", "an interaction is in progress");
    }
    const DetailsRow* target = propertyRow(row);
    if (target == nullptr) {
        return noProperty();
    }
    // While a play session runs, the edit is of the game world (ADR-0053 decision 13); the play
    // flag without a session leaves nothing to edit.
    if (document.playing() && m_session->play() == nullptr) {
        return makeError("editor.command.playing", "the level can't change while playing");
    }
    if (document.opening()) {
        return makeError("editor.document.busy", "a level is being opened");
    }
    const PropertyPath& path = target->path;
    if (Expected<void> editable = detail::checkEditable(*m_session, "ith.entity.set_property", path); !editable) {
        return editable;
    }
    // The same entry ith.entity.set_property records (ADR-0037 rule 6).
    const CommandDesc* command = m_session->commands().find("ith.entity.set_property");
    m_edit.emplace(Edit{
        .path = path,
        .key = target->key,
        .scope = active.transactions.begin(command != nullptr ? command->displayName : std::string("Set Property")),
        .world = active.identity});
    return {};
}

Expected<void> DetailsModel::previewValue(const void* value) {
    if (endForeignEdit()) {
        refresh();
    }
    if (!m_edit.has_value()) {
        return makeError("editor.details.not_editing", "no edit is in progress");
    }
    if (m_edit->cancelled || !m_edit->scope.active()) {
        if (!m_edit->cancelled) {
            m_edit->cancelled = true;
            refresh();
        }
        return makeError("editor.details.edit_cancelled", "the edit was cancelled");
    }
    const PropertyPath& path = m_edit->path;
    OwnedValue copy = OwnedValue::copyOf(path.leafType(), value);
    clampToField(path.leafType(), copy.get(), path.leafField());
    std::vector<EntityPatch> patches;
    for (const Entity entity : m_handles) {
        Patch patch(path.rootType());
        patch.set(path, copy);
        EntityPatch& entityPatch = patches.emplace_back(EntityPatch{.entity = entity});
        entityPatch.changes.push_back(ComponentChange::makePatch(std::move(patch)));
    }
    if (Expected<void> applied = m_edit->scope.apply(patches); !applied) {
        return applied;
    }
    const std::size_t component = componentIndex(path.rootType());
    for (std::vector<OwnedValue>& copies : m_copies) {
        Patch patch(path.rootType());
        patch.set(path, copy);
        static_cast<void>(applyPatch(patch, copies[component].get()));
    }
    buildRows();
    return {};
}

void DetailsModel::commitEdit() {
    if (endForeignEdit() || !m_edit.has_value()) {
        return;
    }
    m_edit->scope.commit();
    m_edit.reset();
    refresh();
}

void DetailsModel::cancelEdit() {
    if (endForeignEdit() || !m_edit.has_value()) {
        return;
    }
    m_edit->scope.cancel();
    m_edit.reset();
    refresh();
}

bool DetailsModel::editing() const noexcept {
    return m_edit.has_value() && m_edit->world == m_session->active().identity;
}

std::string_view DetailsModel::editingKey() const noexcept {
    return m_edit.has_value() && editing() ? std::string_view(m_edit->key) : std::string_view();
}

// --- Entity references ---

std::vector<EntityChoice> DetailsModel::entityChoices(std::string_view filter) const {
    const World& world = m_session->active().world;
    const std::string needle = toLowerAscii(filter);
    std::vector<EntityChoice> choices;
    for (const Entity entity : detail::outlinerOrder(world)) {
        const auto* stable = world.get<const StableId>(entity);
        if (stable == nullptr) {
            continue;
        }
        const std::string* label = detail::labelOf(*m_session, world, entity);
        std::string text = label != nullptr ? *label : stable->id.toString();
        if (needle.empty() || toLowerAscii(text).find(needle) != std::string::npos) {
            choices.push_back(EntityChoice{.id = stable->id, .label = std::move(text)});
        }
    }
    return choices;
}

std::string DetailsModel::entityLabel(Entity entity) const {
    const World& world = m_session->active().world;
    if (stale() || entity.isNull() || !world.isAlive(entity)) {
        return "None";
    }
    if (const std::string* label = detail::labelOf(*m_session, world, entity)) {
        return *label;
    }
    const auto* stable = world.get<const StableId>(entity);
    return stable != nullptr ? stable->id.toString() : std::string("None");
}

Entity DetailsModel::entityOf(const Uuid& id) const {
    return m_session->active().world.findByStableId(id);
}

} // namespace ith
