#include "ith/editor_core/ContentBrowserModel.h"

#include "ith/asset_database/AssetHeader.h"
#include "ith/editor_core/ContentPaths.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/StringUtils.h"
#include "ith/level/LevelFormat.h"
#include "ith/serialization/TextArchive.h"
#include "ith/serialization/Value.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace ith {

namespace {

ITH_LOG_CATEGORY(ContentBrowser);

using serialization::Value;

constexpr std::array<std::string_view, kContentCategoryCount> kCategoryNames = {"Level", "Model", "Texture",
                                                                                "Audio", "Data",  "Other"};
constexpr std::array<std::string_view, kContentCategoryCount> kCategoryIds = {"level", "model", "texture",
                                                                              "audio", "data",  "other"};

// The type category table (spec 3.2). Texture and Audio get their rows with their importers.
struct TypeRow {
    std::string_view type;
    ContentType info;
};

constexpr std::array kTypeTable = {
    TypeRow{kLevelFormat, {.displayName = "Level", .category = ContentCategory::Level}},
    TypeRow{"gltf", {.displayName = "glTF Model", .category = ContentCategory::Model}},
    TypeRow{"ith.input_action", {.displayName = "Input Action", .category = ContentCategory::Data}},
    TypeRow{"ith.input_context", {.displayName = "Input Context", .category = ContentCategory::Data}},
};

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

char lowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The end of the run of digits that starts at `at`.
std::size_t digitsEnd(std::string_view text, std::size_t at) noexcept {
    while (at < text.size() && isDigit(text[at])) {
        ++at;
    }
    return at;
}

// Compares the digit runs a[i, endA) and b[j, endB) by value, then the shorter run first.
int compareDigitRuns(std::string_view a, std::size_t i, std::size_t endA, std::string_view b, std::size_t j,
                     std::size_t endB) noexcept {
    // The value: leading zeros dropped (one digit kept), then the longer number is larger.
    std::size_t startA = i;
    std::size_t startB = j;
    while (startA + 1 < endA && a[startA] == '0') {
        ++startA;
    }
    while (startB + 1 < endB && b[startB] == '0') {
        ++startB;
    }
    const std::size_t lengthA = endA - startA;
    const std::size_t lengthB = endB - startB;
    if (lengthA != lengthB) {
        return lengthA < lengthB ? -1 : 1;
    }
    if (const int value = a.substr(startA, lengthA).compare(b.substr(startB, lengthB)); value != 0) {
        return value < 0 ? -1 : 1;
    }
    if (endA - i != endB - j) {
        return endA - i < endB - j ? -1 : 1;
    }
    return 0;
}

// `path` with `from` (a file or folder) replaced by `to` when `path` is `from` or below it.
std::optional<std::string> remapped(std::string_view path, std::string_view from, std::string_view to) {
    if (from.empty() || !detail::isInContentFolder(path, from)) {
        return std::nullopt;
    }
    return std::string(to) + std::string(path.substr(from.size()));
}

// A native asset's label: its file name without the native extension.
std::string assetLabel(const AssetRecord& record) {
    std::string_view name = detail::contentName(record.path);
    if (!record.imported) {
        for (const std::string_view extension : {kNativeAssetExtension, kLevelExtension}) {
            if (name.size() > extension.size() && endsWithIgnoreCaseAscii(name, extension)) {
                name.remove_suffix(extension.size());
                break;
            }
        }
    }
    return std::string(name);
}

std::string_view viewModeId(ContentViewMode mode) {
    return mode == ContentViewMode::List ? "list" : "grid";
}

std::string_view sortKeyId(ContentSortKey key) {
    return key == ContentSortKey::Type ? "type" : "name";
}

// The member `key` of the object `data`: null when it is missing, an error when it has another kind.
Expected<const Value*> member(const Value& data, std::string_view key, Value::Kind kind) {
    const Value* value = data.find(key);
    if (value != nullptr && value->kind() != kind) {
        return makeError("editor.content_browser.invalid_state", "\"" + std::string(key) + "\" has the wrong type");
    }
    return value;
}

// parseContentBrowserState: any member of the wrong type makes the whole file unreadable.
Expected<ContentBrowserState> parseStateText(std::string_view text, std::string_view source) {
    Expected<Value, serialization::Error> data = serialization::readDocument(
        text, kContentBrowserStateFormat, {.source = source, .maxSize = kMaxContentBrowserStateSize});
    if (!data) {
        return makeError(data.error().code, data.error().toString());
    }
    if (data->kind() != Value::Kind::Object) {
        return makeError("editor.content_browser.invalid_state", "the data is not an object");
    }
    const Expected<const Value*> folder = member(*data, "folder", Value::Kind::String);
    const Expected<const Value*> mode = member(*data, "view", Value::Kind::String);
    const Expected<const Value*> sortBy = member(*data, "sortBy", Value::Kind::String);
    const Expected<const Value*> descending = member(*data, "descending", Value::Kind::Bool);
    const Expected<const Value*> expanded = member(*data, "expanded", Value::Kind::Array);
    const Expected<const Value*> treeWidth = member(*data, "treeWidth", Value::Kind::Number);
    const Expected<const Value*> drawerHeight = member(*data, "drawerHeight", Value::Kind::Number);
    for (const Expected<const Value*>* read :
         {&folder, &mode, &sortBy, &descending, &expanded, &treeWidth, &drawerHeight}) {
        if (!*read) {
            return makeError(read->error().code, std::string(source) + ": " + read->error().message);
        }
    }
    ContentBrowserState state;
    state.folder = *folder != nullptr ? (*folder)->asString() : std::string();
    state.view.mode = *mode != nullptr && (*mode)->asString() == "list" ? ContentViewMode::List : ContentViewMode::Grid;
    state.view.sortBy =
        *sortBy != nullptr && (*sortBy)->asString() == "type" ? ContentSortKey::Type : ContentSortKey::Name;
    state.view.descending = *descending != nullptr && (*descending)->asBool();
    if (*expanded != nullptr) {
        for (const Value& element : (*expanded)->elements()) {
            if (element.kind() == Value::Kind::String) {
                state.expanded.push_back(element.asString());
            }
        }
    }
    if (*treeWidth != nullptr) {
        state.treeWidth = std::clamp((*treeWidth)->toNumber<float>().value_or(kDefaultContentTreeWidth), 0.05f, 0.95f);
    }
    if (*drawerHeight != nullptr) {
        state.drawerHeight = std::max((*drawerHeight)->toNumber<float>().value_or(0.0f), 0.0f);
    }
    return state;
}

// The badge text of a scan problem at its own path.
std::string badgeMessage(const AssetProblem& problem) {
    switch (problem.kind) {
        case AssetProblemKind::MissingSidecar:
            return "Not imported";
        case AssetProblemKind::OrphanSidecar:
            return "Source file missing";
        case AssetProblemKind::DuplicateId:
            return "Duplicate id of " + problem.otherPath;
        default:
            return problem.message;
    }
}

} // namespace

std::string_view displayName(ContentCategory category) noexcept {
    const auto index = static_cast<std::size_t>(category);
    return index < kCategoryNames.size() ? kCategoryNames[index] : kCategoryNames.back();
}

std::string_view categoryId(ContentCategory category) noexcept {
    const auto index = static_cast<std::size_t>(category);
    return index < kCategoryIds.size() ? kCategoryIds[index] : kCategoryIds.back();
}

std::optional<ContentCategory> parseCategory(std::string_view id) noexcept {
    for (std::size_t i = 0; i < kCategoryIds.size(); ++i) {
        if (kCategoryIds[i] == id) {
            return static_cast<ContentCategory>(i);
        }
    }
    return std::nullopt;
}

Expected<ContentBrowserState> parseContentBrowserState(std::string_view text, std::string_view source) {
    return parseStateText(text, source);
}

ContentType contentTypeOf(std::string_view type) noexcept {
    for (const TypeRow& row : kTypeTable) {
        if (row.type == type) {
            return row.info;
        }
    }
    return ContentType{.displayName = type, .category = ContentCategory::Other};
}

int naturalCompare(std::string_view a, std::string_view b) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            const std::size_t endA = digitsEnd(a, i);
            const std::size_t endB = digitsEnd(b, j);
            if (const int order = compareDigitRuns(a, i, endA, b, j, endB); order != 0) {
                return order;
            }
            i = endA;
            j = endB;
            continue;
        }
        const auto ca = static_cast<unsigned char>(lowerAscii(a[i]));
        const auto cb = static_cast<unsigned char>(lowerAscii(b[j]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < a.size()) {
        return 1;
    }
    return j < b.size() ? -1 : 0;
}

ContentKey keyOf(const ContentItem& item) {
    if (item.kind == ContentItemKind::Asset) {
        return ContentKey{.id = item.id, .path = {}};
    }
    return ContentKey{.id = AssetId(), .path = item.path};
}

ContentNameParts nameParts(const ContentItem& item) {
    const std::string_view name = detail::contentName(item.path);
    if (item.kind == ContentItemKind::Folder) {
        return ContentNameParts{.editable = std::string(name), .extension = {}};
    }
    if (item.kind == ContentItemKind::Asset && item.label.size() < name.size() && name.starts_with(item.label) &&
        isNativeAssetPath(name)) {
        return ContentNameParts{.editable = item.label, .extension = std::string(name.substr(item.label.size()))};
    }
    const std::string_view extension = fs::extension(name);
    return ContentNameParts{.editable = std::string(name.substr(0, name.size() - extension.size())),
                            .extension = std::string(extension)};
}

Expected<void> checkContentName(std::string_view name) {
    const auto invalid = [name](std::string_view reason) {
        return makeError("editor.content.invalid_name", "'" + std::string(name) + "' " + std::string(reason));
    };
    if (name.empty()) {
        return makeError("editor.content.invalid_name", "a name can't be empty");
    }
    if (name.find_first_of("/\\") != std::string_view::npos) {
        return invalid("holds / or \\, which separate folders");
    }
    if (name.size() > kMaxContentNameSize) {
        return invalid("is longer than " + std::to_string(kMaxContentNameSize) + " bytes");
    }
    if (name.back() == '.' || name.back() == ' ') {
        return invalid("ends in '.' or a space, which Windows drops");
    }
    if (endsWithIgnoreCaseAscii(name, kSidecarSuffix)) {
        return invalid("ends in .meta, which names sidecars");
    }
    if (!isValidContentPath(name)) {
        return invalid("holds one of < > : \" | ? * or a control character, isn't valid UTF-8, or is a device "
                       "name such as CON or COM1");
    }
    return {};
}

std::string_view numberedNameBase(std::string_view editable) noexcept {
    std::size_t digits = editable.size();
    while (digits > 0 && isDigit(editable[digits - 1])) {
        --digits;
    }
    if (digits == editable.size() || digits < 2 || editable[digits - 1] != '_') {
        return editable;
    }
    return editable.substr(0, digits - 1);
}

ContentBrowserModel::ContentBrowserModel(AssetDatabase* assets, std::string stateFile)
    : m_assets(assets), m_stateFile(std::move(stateFile)) {
    load();
    rebuildTree();
}

Expected<void> ContentBrowserModel::rescan() {
    if (m_assets == nullptr) {
        return makeError("editor.document.no_asset_database", "no project is open");
    }
    Expected<std::vector<AssetProblem>> problems = m_assets->scan();
    if (!problems) {
        return makeUnexpected(std::move(problems).error());
    }
    m_problems = std::move(*problems);
    for (const AssetProblem& problem : m_problems) {
        if (problem.kind == AssetProblemKind::DuplicateId && m_assets->findByPath(problem.path) != nullptr) {
            ITH_LOG_INFO(ContentBrowser, "{}: {}", problem.path, problem.message);
        }
    }
    refresh();
    return {};
}

void ContentBrowserModel::refresh() {
    m_items.clear();
    m_byPath.clear();
    m_byId.clear();
    m_children.clear();
    m_folders = {std::string()};
    const auto add = [this](ContentItem item) {
        m_byPath.emplace(item.path, m_items.size());
        if (item.kind == ContentItemKind::Asset) {
            m_byId.emplace(item.id, m_items.size());
        }
        m_items.push_back(std::move(item));
    };
    if (m_assets != nullptr) {
        for (const std::string& folder : m_assets->folders()) {
            m_folders.insert(folder);
            add(ContentItem{.kind = ContentItemKind::Folder,
                            .path = folder,
                            .folder = detail::contentParent(folder),
                            .label = std::string(detail::contentName(folder))});
        }
        for (const AssetRecord* record : m_assets->records()) {
            const ContentType type = contentTypeOf(record->type);
            add(ContentItem{.kind = ContentItemKind::Asset,
                            .path = record->path,
                            .folder = detail::contentParent(record->path),
                            .id = record->id,
                            .label = assetLabel(*record),
                            .type = record->type,
                            .category = type.category});
        }
    }
    // Problems become badges on the items they are about, or entries of their own (spec 3.2).
    const auto itemAt = [this](const std::string& path) -> ContentItem* {
        const auto found = m_byPath.find(path);
        return found != m_byPath.end() ? &m_items[found->second] : nullptr;
    };
    for (const AssetProblem& problem : m_problems) {
        const bool resolvedDuplicate = problem.kind == AssetProblemKind::DuplicateId && m_assets != nullptr &&
                                       m_assets->findByPath(problem.path) != nullptr;
        if (resolvedDuplicate || problem.path.empty()) {
            continue; // logged by rescan(); a problem of the root itself is in problems() only
        }
        if (problem.kind == AssetProblemKind::CaseConflict) {
            for (const auto& [path, other] :
                 {std::pair{&problem.path, &problem.otherPath}, std::pair{&problem.otherPath, &problem.path}}) {
                if (ContentItem* item = itemAt(*path); item != nullptr) {
                    item->problems.push_back(ContentBadge{
                        .kind = problem.kind, .message = "Differs only in case from " + *other, .otherPath = *other});
                }
            }
            continue;
        }
        const ContentBadge badge{
            .kind = problem.kind, .message = badgeMessage(problem), .otherPath = problem.otherPath};
        if (ContentItem* item = itemAt(problem.path); item != nullptr) {
            item->problems.push_back(badge);
        } else if (problem.kind != AssetProblemKind::FormatMismatch) {
            add(ContentItem{.kind = ContentItemKind::Problem,
                            .path = problem.path,
                            .folder = detail::contentParent(problem.path),
                            .label = std::string(detail::contentName(problem.path)),
                            .problems = {badge}});
        }
    }
    m_lowerLabels.clear();
    m_lowerLabels.reserve(m_items.size());
    for (std::size_t index = 0; index < m_items.size(); ++index) {
        m_lowerLabels.push_back(toLowerAscii(m_items[index].label));
        m_children[m_items[index].folder].push_back(index);
    }
    // Every item's place in name order and in type order, once: sorting what is shown then sorts
    // plain numbers (sortShown packs a place and an index into 63 bits).
    ITH_ASSERT(m_items.size() < (std::size_t{1} << 31U));
    std::vector<std::uint32_t> order(m_items.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = static_cast<std::uint32_t>(index);
    }
    std::ranges::sort(order, [this](std::uint32_t a, std::uint32_t b) {
        const ContentItem& first = m_items[a];
        const ContentItem& second = m_items[b];
        if (const int natural = naturalCompare(first.label, second.label); natural != 0) {
            return natural < 0;
        }
        if (first.label != second.label) {
            return first.label < second.label;
        }
        return first.path < second.path;
    });
    m_nameRanks.assign(m_items.size(), 0);
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        m_nameRanks[order[rank]] = static_cast<std::uint32_t>(rank);
    }
    std::ranges::sort(order, [this](std::uint32_t a, std::uint32_t b) {
        const ContentItem& first = m_items[a];
        const ContentItem& second = m_items[b];
        if (const int byCategory = displayName(first.category).compare(displayName(second.category)); byCategory != 0) {
            return byCategory < 0;
        }
        const std::string_view typeA = contentTypeOf(first.type).displayName;
        const std::string_view typeB = contentTypeOf(second.type).displayName;
        if (const int byType = typeA.compare(typeB); byType != 0) {
            return byType < 0;
        }
        return m_nameRanks[a] < m_nameRanks[b];
    });
    m_typeRanks.assign(m_items.size(), 0);
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        m_typeRanks[order[rank]] = static_cast<std::uint32_t>(rank);
    }

    while (!folderExists(m_folder)) {
        m_folder = detail::contentParent(m_folder);
    }
    std::erase_if(m_expanded, [this](const std::string& folder) { return !folderExists(folder); });
    rebuildTree();
    rebuildView();
    pruneSelection();
    dropStalePrompt();
    writeState();
}

const ContentItem* ContentBrowserModel::find(const ContentKey& key) const {
    if (!key.id.isNil()) {
        const auto found = m_byId.find(key.id);
        return found != m_byId.end() ? &m_items[found->second] : nullptr;
    }
    const ContentItem* item = findByPath(key.path);
    return item != nullptr && item->kind != ContentItemKind::Asset ? item : nullptr;
}

const ContentItem* ContentBrowserModel::findByPath(std::string_view path) const {
    const auto found = m_byPath.find(std::string(path));
    return found != m_byPath.end() ? &m_items[found->second] : nullptr;
}

bool ContentBrowserModel::folderExists(std::string_view path) const {
    return m_folders.contains(std::string(path));
}

Expected<void> ContentBrowserModel::navigate(std::string_view folder) {
    if (!folderExists(folder)) {
        return makeError("editor.content.not_found", "'" + std::string(folder) + "' is not a folder in Content");
    }
    m_folder = std::string(folder); // `folder` may view m_folder itself: only m_folder from here on
    expandAncestors(m_folder);
    m_search.clear();
    clearSelection();
    rebuildTree();
    rebuildView();
    writeState();
    return {};
}

bool ContentBrowserModel::expandAncestors(std::string_view folder) {
    bool added = false;
    for (std::string ancestor = detail::contentParent(folder); !ancestor.empty();
         ancestor = detail::contentParent(ancestor)) {
        added = m_expanded.insert(ancestor).second || added;
    }
    return added;
}

void ContentBrowserModel::revealInTree(std::string_view folder) {
    if (!folderExists(folder) || !expandAncestors(folder)) {
        return;
    }
    rebuildTree();
    writeState();
    changed();
}

bool ContentBrowserModel::navigateUp() {
    if (m_folder.empty()) {
        return false;
    }
    return navigate(detail::contentParent(m_folder)).hasValue();
}

bool ContentBrowserModel::isExpanded(std::string_view folder) const {
    return folder.empty() || m_expanded.contains(std::string(folder));
}

void ContentBrowserModel::setExpanded(std::string_view folder, bool expanded) {
    if (folder.empty() || !folderExists(folder) || isExpanded(folder) == expanded) {
        return;
    }
    if (expanded) {
        m_expanded.insert(std::string(folder));
    } else {
        m_expanded.erase(std::string(folder));
    }
    rebuildTree();
    writeState();
    changed();
}

void ContentBrowserModel::setSearch(std::string text) {
    if (text.size() > kMaxContentSearchSize) {
        // Cut before the UTF-8 character that doesn't fit whole: back over continuation bytes.
        std::size_t size = kMaxContentSearchSize;
        while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) {
            --size;
        }
        text.resize(size);
    }
    if (text == m_search) {
        return;
    }
    m_search = std::move(text);
    rebuildView();
    pruneSelection();
}

void ContentBrowserModel::pointerPressed(float x, float y) noexcept {
    m_keyboardFocus =
        std::ranges::any_of(m_screenRects, [x, y](const ContentScreenRect& rect) { return rect.contains(x, y); });
    m_treeFocus =
        std::ranges::any_of(m_treeRects, [x, y](const ContentScreenRect& rect) { return rect.contains(x, y); });
}

void ContentBrowserModel::setFilter(ContentFilter filter) {
    if (filter == m_filter) {
        return;
    }
    m_filter = filter;
    rebuildView();
    pruneSelection();
}

bool ContentBrowserModel::searching() const {
    return !m_filter.empty() || std::ranges::any_of(m_search, [](char c) { return c != ' '; });
}

std::optional<std::size_t> ContentBrowserModel::shownIndexOf(const ContentKey& key) const {
    // A scan: commands and key presses ask for single items.
    for (std::size_t index = 0; index < m_shown.size(); ++index) {
        if (keyOf(*m_shown[index]) == key) {
            return index;
        }
    }
    return std::nullopt;
}

void ContentBrowserModel::setView(const ContentViewSettings& view) {
    if (view == m_view) {
        return;
    }
    m_view = view;
    sortShown();
    writeState();
    changed();
}

Expected<void> ContentBrowserModel::select(std::span<const ContentKey> items, ContentSelect how) {
    std::unordered_map<ContentKey, std::size_t, ContentKeyHash> shownIndex;
    shownIndex.reserve(m_shown.size());
    for (std::size_t index = 0; index < m_shown.size(); ++index) {
        shownIndex.emplace(keyOf(*m_shown[index]), index);
    }
    for (const ContentKey& key : items) {
        if (!shownIndex.contains(key)) {
            return makeError("editor.content.not_found",
                             "'" + (key.id.isNil() ? key.path : key.id.toString()) + "' is not shown");
        }
    }
    if (how == ContentSelect::Range || how == ContentSelect::AddRange) {
        const auto anchor = m_anchor.has_value() ? shownIndex.find(*m_anchor) : shownIndex.end();
        if (anchor != shownIndex.end() && !items.empty()) {
            if (how == ContentSelect::Range) {
                m_selection.clear();
                m_selected.clear();
            }
            selectRange(anchor->second, shownIndex.at(items.back()));
            m_focus = items.back();
            changed();
            return {};
        }
        how = ContentSelect::Replace; // without an anchor, Shift acts like a plain click
    }
    switch (how) {
        case ContentSelect::Replace:
            m_selection.clear();
            m_selected.clear();
            [[fallthrough]];
        case ContentSelect::Add:
            for (const ContentKey& key : items) {
                appendSelected(key);
            }
            break;
        case ContentSelect::Toggle:
            for (const ContentKey& key : items) {
                if (isSelected(key)) {
                    deselect(key);
                } else {
                    appendSelected(key);
                }
            }
            break;
        case ContentSelect::Remove:
            for (const ContentKey& key : items) {
                deselect(key);
            }
            break;
        case ContentSelect::Range:
        case ContentSelect::AddRange:
            break; // handled above
    }
    if (how != ContentSelect::Remove) {
        m_anchor = items.empty() ? std::nullopt : std::optional<ContentKey>(items.back());
        m_focus = m_anchor;
    }
    changed();
    return {};
}

void ContentBrowserModel::deselect(const ContentKey& key) {
    if (m_selected.erase(key) > 0) {
        std::erase(m_selection, key);
    }
}

void ContentBrowserModel::appendSelected(const ContentKey& key) {
    deselect(key);
    m_selection.push_back(key);
    m_selected.insert(key);
}

void ContentBrowserModel::selectRange(std::size_t from, std::size_t to) {
    // From the anchor towards the clicked item; items already selected keep their place, and the
    // clicked item comes last, so it is primary.
    for (std::size_t step = 0;; ++step) {
        const std::size_t index = from <= to ? from + step : from - step;
        if (const ContentKey key = keyOf(*m_shown[index]); !isSelected(key)) {
            appendSelected(key);
        }
        if (index == to) {
            break;
        }
    }
    appendSelected(keyOf(*m_shown[to]));
}

void ContentBrowserModel::selectAll() {
    m_selection.clear();
    m_selected.clear();
    for (const ContentItem* item : m_shown) {
        m_selection.push_back(keyOf(*item));
        m_selected.insert(m_selection.back());
    }
    changed();
}

void ContentBrowserModel::clearSelection() {
    m_selection.clear();
    m_selected.clear();
    m_anchor.reset();
    m_focus.reset();
    changed();
}

void ContentBrowserModel::setDrawerOpen(bool open) {
    if (open != m_drawerOpen) {
        m_drawerOpen = open;
        changed();
    }
}

void ContentBrowserModel::setTreeWidth(float fraction) {
    const float clamped = std::clamp(fraction, 0.05f, 0.95f);
    if (clamped != m_treeWidth) {
        m_treeWidth = clamped;
        writeState();
        changed();
    }
}

void ContentBrowserModel::setDrawerHeight(float height) {
    const float value = std::max(height, 0.0f);
    if (value != m_drawerHeight) {
        m_drawerHeight = value;
        writeState();
        changed();
    }
}

void ContentBrowserModel::setPrompt(ContentPrompt prompt) {
    m_prompt = std::move(prompt);
    changed();
}

void ContentBrowserModel::dropStalePrompt() {
    // A rename whose item is gone (deleted, or gone after a Rescan) has nothing left to rename.
    if (m_prompt.has_value() && m_prompt->kind == ContentPromptKind::Rename && find(m_prompt->item) == nullptr) {
        m_prompt.reset();
    }
}

void ContentBrowserModel::clearPrompt() {
    if (m_prompt.has_value()) {
        m_prompt.reset();
        changed();
    }
}

void ContentBrowserModel::movedPath(std::string_view from, std::string_view to) {
    if (from.empty() || from == to) {
        return;
    }
    const bool caseOnly = equalsIgnoreCaseAscii(from, to);
    if (std::optional<std::string> folder = remapped(m_folder, from, to)) {
        m_folder = std::move(*folder);
    }
    std::unordered_set<std::string> expanded;
    for (const std::string& folder : m_expanded) {
        expanded.insert(remapped(folder, from, to).value_or(folder));
    }
    m_expanded = std::move(expanded);
    const auto remapKey = [from, to](ContentKey& key) {
        if (key.id.isNil()) {
            key.path = remapped(key.path, from, to).value_or(key.path);
        }
    };
    for (ContentKey& key : m_selection) {
        remapKey(key);
    }
    m_selected = {m_selection.begin(), m_selection.end()};
    for (std::optional<ContentKey>* key : {&m_anchor, &m_focus}) {
        if (key->has_value()) {
            remapKey(**key);
        }
    }
    if (m_prompt.has_value() && m_prompt->kind == ContentPromptKind::Rename) {
        remapKey(m_prompt->item);
        if (Value* path = m_prompt->details.kind() == Value::Kind::Object ? m_prompt->details.find("path") : nullptr;
            path != nullptr && path->kind() == Value::Kind::String) {
            *path = Value::string(remapped(path->asString(), from, to).value_or(path->asString()));
        }
    }
    std::erase_if(m_problems, [from, caseOnly](const AssetProblem& problem) {
        const bool moved = detail::isInContentFolder(problem.path, from);
        const bool otherMoved = !problem.otherPath.empty() && detail::isInContentFolder(problem.otherPath, from);
        return problem.kind == AssetProblemKind::CaseConflict && moved != otherMoved && !caseOnly;
    });
    for (AssetProblem& problem : m_problems) {
        problem.path = remapped(problem.path, from, to).value_or(problem.path);
        if (!problem.otherPath.empty()) {
            problem.otherPath = remapped(problem.otherPath, from, to).value_or(problem.otherPath);
        }
    }
    std::ranges::stable_sort(m_problems, [](const AssetProblem& a, const AssetProblem& b) { return a.path < b.path; });
    changed();
}

void ContentBrowserModel::removedPath(std::string_view path) {
    if (path.empty()) {
        return;
    }
    std::erase_if(m_problems,
                  [path](const AssetProblem& problem) { return detail::isInContentFolder(problem.path, path); });
    changed();
}

void ContentBrowserModel::addProblems(std::span<const AssetProblem> problems) {
    for (const AssetProblem& problem : problems) {
        if (std::ranges::find(m_problems, problem) == m_problems.end()) {
            m_problems.push_back(problem);
        }
    }
    std::ranges::stable_sort(m_problems, [](const AssetProblem& a, const AssetProblem& b) { return a.path < b.path; });
    changed();
}

void ContentBrowserModel::rekey(const ContentKey& from, const ContentKey& to) {
    for (ContentKey& key : m_selection) {
        if (key == from) {
            key = to;
        }
    }
    m_selected = {m_selection.begin(), m_selection.end()};
    for (std::optional<ContentKey>* key : {&m_anchor, &m_focus}) {
        if (*key == from) {
            *key = to;
        }
    }
    changed();
}

void ContentBrowserModel::selectShown(std::span<const ContentKey> keys) {
    std::vector<ContentKey> shown;
    for (const ContentKey& key : keys) {
        if (shownIndexOf(key).has_value()) {
            shown.push_back(key);
        }
    }
    static_cast<void>(select(shown, ContentSelect::Replace));
}

void ContentBrowserModel::rebuildTree() {
    // The subfolders of each folder in natural order, then a depth-first walk.
    std::unordered_map<std::string, std::vector<const std::string*>> subfolders;
    for (const std::string& folder : m_folders) {
        if (!folder.empty()) {
            subfolders[detail::contentParent(folder)].push_back(&folder);
        }
    }
    for (auto& [parent, list] : subfolders) {
        std::ranges::sort(list, [](const std::string* a, const std::string* b) {
            const int order = naturalCompare(detail::contentName(*a), detail::contentName(*b));
            return order != 0 ? order < 0 : *a < *b;
        });
    }
    m_treeRows.clear();
    const auto visit = [this, &subfolders](const auto& self, const std::string& folder, std::uint32_t depth) -> void {
        const auto children = subfolders.find(folder);
        const bool hasChildren = children != subfolders.end();
        const bool expanded = isExpanded(folder);
        m_treeRows.push_back(
            ContentFolderRow{.path = folder,
                             .name = folder.empty() ? std::string("Content") : std::string(detail::contentName(folder)),
                             .depth = depth,
                             .hasChildren = hasChildren,
                             .expanded = expanded});
        if (hasChildren && expanded) {
            for (const std::string* child : children->second) {
                self(self, *child, depth + 1);
            }
        }
    };
    visit(visit, std::string(), 0);
}

bool ContentBrowserModel::matches(const ContentItem& item, std::span<const std::string> words) const {
    if (!m_filter.empty()) {
        const bool inCategory = item.kind == ContentItemKind::Asset && m_filter.has(item.category);
        if (!inCategory && !(m_filter.problems && item.hasProblems())) {
            return false;
        }
    }
    const std::string& label = m_lowerLabels[static_cast<std::size_t>(&item - m_items.data())];
    return std::ranges::all_of(words,
                               [&label](const std::string& word) { return label.find(word) != std::string::npos; });
}

void ContentBrowserModel::rebuildView() {
    m_shown.clear();
    if (!searching()) {
        if (const auto children = m_children.find(m_folder); children != m_children.end()) {
            m_shown.reserve(children->second.size());
            for (const std::size_t index : children->second) {
                m_shown.push_back(&m_items[index]);
            }
        }
    } else {
        std::vector<std::string> words;
        for (const std::string_view word : split(m_search, ' ')) {
            if (!word.empty()) {
                words.push_back(toLowerAscii(word));
            }
        }
        for (const ContentItem& item : m_items) {
            if (item.kind != ContentItemKind::Folder && detail::isInContentFolder(item.folder, m_folder) &&
                matches(item, words)) {
                m_shown.push_back(&item);
            }
        }
    }
    m_shownProblems = static_cast<std::size_t>(
        std::ranges::count_if(m_shown, [](const ContentItem* item) { return item->hasProblems(); }));
    sortShown();
    ++m_viewBuilds;
    changed();
}

void ContentBrowserModel::sortShown() {
    // Folders first in either direction; descending reverses each group. Each item's place is known
    // (refresh), so one number per item says where it goes: the group in bit 63, the place in bits
    // 32 to 62 and the item's index in m_items below. Plain numbers sort without a comparator; a
    // comparator over the item pointers came out unsorted under MSVC 19.52's optimizer.
    const std::vector<std::uint32_t>& ranks = m_view.sortBy == ContentSortKey::Type ? m_typeRanks : m_nameRanks;
    std::vector<std::uint64_t> keys;
    keys.reserve(m_shown.size());
    for (const ContentItem* item : m_shown) {
        const auto index = static_cast<std::size_t>(item - m_items.data());
        const std::uint64_t group = item->kind == ContentItemKind::Folder ? 0 : 1;
        const std::uint64_t place = m_view.descending ? m_items.size() - 1 - ranks[index] : ranks[index];
        keys.push_back(group << 63U | place << 32U | index);
    }
    std::ranges::sort(keys);
    for (std::size_t at = 0; at < keys.size(); ++at) {
        m_shown[at] = &m_items[static_cast<std::size_t>(keys[at] & 0xFFFF'FFFFU)];
    }
}

void ContentBrowserModel::pruneSelection() {
    if (m_selection.empty() && !m_anchor.has_value() && !m_focus.has_value()) {
        return;
    }
    std::unordered_set<ContentKey, ContentKeyHash> shown;
    shown.reserve(m_shown.size());
    for (const ContentItem* item : m_shown) {
        shown.insert(keyOf(*item));
    }
    const std::size_t before = m_selection.size();
    std::erase_if(m_selection, [&shown](const ContentKey& key) { return !shown.contains(key); });
    m_selected = {m_selection.begin(), m_selection.end()};
    const bool anchorGone = m_anchor.has_value() && !shown.contains(*m_anchor);
    const bool focusGone = m_focus.has_value() && !shown.contains(*m_focus);
    if (anchorGone) {
        m_anchor.reset();
    }
    if (focusGone) {
        m_focus.reset();
    }
    if (before != m_selection.size() || anchorGone || focusGone) {
        changed();
    }
}

void ContentBrowserModel::load() {
    if (m_stateFile.empty()) {
        return;
    }
    const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(m_stateFile);
    if (info && !info->has_value()) {
        // A fresh project has none yet: the defaults, which are written only once something changes.
        m_written = stateText();
        return;
    }
    Expected<std::string> text = info ? fs::readFile(m_stateFile, kMaxContentBrowserStateSize)
                                      : Expected<std::string>(makeUnexpected(info.error()));
    Expected<ContentBrowserState> state = text ? parseContentBrowserState(*text, m_stateFile)
                                               : Expected<ContentBrowserState>(makeUnexpected(std::move(text).error()));
    if (!state) {
        ITH_LOG_WARNING(ContentBrowser, "the Content Browser's state can't be read; it starts with the defaults: {}",
                        state.error().message);
        return;
    }
    m_folder = std::move(state->folder);
    m_view = state->view;
    m_expanded = {state->expanded.begin(), state->expanded.end()};
    m_treeWidth = state->treeWidth;
    m_drawerHeight = state->drawerHeight;
    m_written = stateText();
}

std::optional<std::string> ContentBrowserModel::stateText() const {
    std::vector<std::string> expanded(m_expanded.begin(), m_expanded.end());
    std::ranges::sort(expanded);
    Value list = Value::array();
    for (std::string& folder : expanded) {
        list.elements().push_back(Value::string(std::move(folder)));
    }
    Value data = Value::object();
    data.set("folder", Value::string(m_folder));
    data.set("view", Value::string(std::string(viewModeId(m_view.mode))));
    data.set("sortBy", Value::string(std::string(sortKeyId(m_view.sortBy))));
    data.set("descending", Value::boolean(m_view.descending));
    data.set("expanded", std::move(list));
    data.set("treeWidth", Value::real(m_treeWidth));
    data.set("drawerHeight", Value::real(m_drawerHeight));
    Expected<std::string, serialization::Error> text = serialization::writeDocument(kContentBrowserStateFormat, data);
    if (!text) {
        ITH_LOG_WARNING(ContentBrowser, "the Content Browser's state can't be written: {}", text.error().toString());
        return std::nullopt;
    }
    return std::move(*text);
}

void ContentBrowserModel::writeState() {
    if (m_stateFile.empty()) {
        return;
    }
    std::optional<std::string> text = stateText();
    if (!text.has_value() || m_written == text) {
        return;
    }
    Expected<void> written = fs::writeFileAtomic(m_stateFile, *text, {.durability = fs::Durability::Atomic});
    if (!written) {
        if (!m_writeFailed) {
            ITH_LOG_WARNING(ContentBrowser, "the Content Browser's state can't be written: {}",
                            written.error().message);
        }
        m_writeFailed = true;
        return;
    }
    m_writeFailed = false;
    m_written = std::move(*text);
}

} // namespace ith
