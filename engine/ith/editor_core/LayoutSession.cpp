#include "ith/editor_core/LayoutSession.h"

#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/StringUtils.h"

#include <algorithm>

namespace ith {

namespace {

ITH_LOG_CATEGORY(EditorLayout);

constexpr std::string_view kExtension = ".json";
constexpr std::size_t kMaxNameLength = 64;

void logWarnings(const std::vector<Error>& warnings) {
    for (const Error& warning : warnings) {
        ITH_LOG_WARNING(EditorLayout, "{}: {}", warning.code, warning.message);
    }
}

// 1 to 64 letters, digits, spaces, '_' and '-', not starting or ending with a space.
bool isValidName(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength || name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
               c == '-';
    });
}

std::string layoutPath(std::string_view name) {
    return std::string(kLayoutFolder) + "/" + std::string(name) + std::string(kExtension);
}

// The default layout's name in a merged Editor.json, or the engine's when it names none.
std::string defaultIn(const serialization::Value& editor) {
    const serialization::Value* layouts = editor.find("layouts");
    const serialization::Value* name = layouts != nullptr ? layouts->find("defaultName") : nullptr;
    if (name != nullptr && name->kind() == serialization::Value::Kind::String) {
        return name->asString();
    }
    return EditorLayoutSettings{}.defaultName;
}

} // namespace

LayoutSession::LayoutSession(const PanelRegistry& panels, Config* config, std::string sessionFile)
    : m_panels(&panels), m_config(config), m_sessionFile(std::move(sessionFile)) {}

Expected<void> LayoutSession::needConfig() const {
    if (m_config == nullptr) {
        return makeError("editor.layout.no_config", "the editor has no configuration to keep layouts in");
    }
    return {};
}

Expected<std::size_t> LayoutSession::userLayer() const {
    if (Expected<void> config = needConfig(); !config) {
        return makeUnexpected(std::move(config).error());
    }
    const std::size_t layer = m_config->layers().size() - 1;
    if (!m_config->layers()[layer].writable) {
        return makeError("editor.layout.no_config", "the highest configuration layer is not writable");
    }
    return layer;
}

void LayoutSession::readBack() {
    if (m_readBack) {
        m_readBack();
    }
}

void LayoutSession::replace(EditorLayout layout, std::string name) {
    m_current = std::move(layout);
    m_name = std::move(name);
    ++m_generation;
    ++m_changes;
    saveSession();
}

void LayoutSession::observe(EditorLayout layout) {
    normalizeLayout(layout, *m_panels, nullptr);
    if (layout == m_current) {
        return;
    }
    m_current = std::move(layout);
    ++m_changes;
    saveSession();
}

void LayoutSession::saveSession() {
    if (m_sessionFile.empty() || (m_written.has_value() && *m_written == m_current)) {
        return;
    }
    Expected<std::string> text = writeLayout(m_current);
    Expected<void> written = text ? fs::writeFileAtomic(m_sessionFile, *text, {.durability = fs::Durability::Atomic})
                                  : Expected<void>(makeUnexpected(text.error()));
    if (!written) {
        if (!m_sessionWriteFailed) {
            ITH_LOG_WARNING(EditorLayout, "the session layout can't be written: {}", written.error().message);
        }
        m_sessionWriteFailed = true;
        return;
    }
    m_sessionWriteFailed = false;
    m_written = m_current;
}

Expected<std::optional<LayoutSession::Found>> LayoutSession::find(std::string_view name) {
    if (Expected<void> config = needConfig(); !config) {
        return makeUnexpected(std::move(config).error());
    }
    Expected<std::vector<ConfigFile>> files = m_config->listFiles(kLayoutFolder);
    if (!files) {
        return makeUnexpected(std::move(files).error());
    }
    for (ConfigFile& file : *files) {
        if (!file.name.ends_with(kExtension)) {
            continue;
        }
        const std::string_view stem = std::string_view(file.name).substr(0, file.name.size() - kExtension.size());
        if (equalsIgnoreCaseAscii(stem, name)) {
            return std::optional<Found>(
                Found{.name = std::string(stem), .layer = file.layer, .path = std::move(file.path)});
        }
    }
    return std::optional<Found>();
}

Expected<EditorLayout> LayoutSession::loadFile(const Found& found, std::vector<Error>& warnings) {
    Expected<std::string> text = fs::readFile(found.path, kMaxLayoutFileSize);
    if (!text) {
        return makeError("editor.layout.invalid",
                         "the layout '" + found.name + "' can't be read: " + text.error().message);
    }
    return readLayout(*text, layoutPath(found.name), *m_panels, &warnings);
}

LayoutLoad LayoutSession::restore() {
    if (!m_sessionFile.empty()) {
        const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(m_sessionFile);
        if (info && info->has_value()) {
            std::vector<Error> warnings;
            Expected<std::string> text = fs::readFile(m_sessionFile, kMaxLayoutFileSize);
            Expected<EditorLayout> layout = text ? readLayout(*text, m_sessionFile, *m_panels, &warnings)
                                                 : Expected<EditorLayout>(makeUnexpected(text.error()));
            if (layout) {
                logWarnings(warnings);
                m_written = *layout;
                replace(std::move(*layout), {});
                return LayoutLoad{.name = {}, .warnings = std::move(warnings)};
            }
            ITH_LOG_WARNING(EditorLayout, "the session layout is skipped: {}", layout.error().message);
        }
    }
    return reset();
}

Expected<LayoutLoad> LayoutSession::load(std::string_view name) {
    readBack();
    Expected<std::optional<Found>> found = find(name);
    if (!found) {
        return makeUnexpected(std::move(found).error());
    }
    if (!found->has_value()) {
        return makeError("editor.layout.not_found", "no layout is called '" + std::string(name) + "'");
    }
    std::vector<Error> warnings;
    Expected<EditorLayout> layout = loadFile(**found, warnings);
    if (!layout) {
        return makeUnexpected(std::move(layout).error());
    }
    logWarnings(warnings);
    replace(std::move(*layout), (*found)->name);
    return LayoutLoad{.name = m_name, .warnings = std::move(warnings)};
}

Expected<std::string> LayoutSession::saveAs(std::string_view name, bool overwrite) {
    if (!isValidName(name)) {
        return makeError("editor.layout.invalid_name",
                         "'" + std::string(name) +
                             "' is not a layout name: 1 to 64 letters, digits, spaces, '_' and '-', without spaces "
                             "at either end");
    }
    Expected<std::size_t> user = userLayer();
    if (!user) {
        return makeUnexpected(std::move(user).error());
    }
    readBack();
    Expected<std::optional<Found>> found = find(name);
    if (!found) {
        return makeUnexpected(std::move(found).error());
    }
    std::string fileName(name);
    if (found->has_value()) {
        if ((*found)->layer != *user) {
            return makeError("editor.layout.reserved_name",
                             "the layout '" + (*found)->name + "' comes with the engine or the project");
        }
        if (!overwrite) {
            return makeError("editor.layout.exists", "a layout called '" + (*found)->name + "' exists");
        }
        fileName = (*found)->name; // one file per name, whatever its case
    }
    Expected<std::string> text = writeLayout(m_current);
    if (!text) {
        return makeUnexpected(std::move(text).error());
    }
    if (Expected<void> written = m_config->writeFile(m_config->layers()[*user].name, layoutPath(fileName), *text);
        !written) {
        return makeUnexpected(std::move(written).error());
    }
    m_name = fileName;
    return fileName;
}

Expected<std::string> LayoutSession::setDefault(std::string_view name) {
    Expected<std::size_t> user = userLayer();
    if (!user) {
        return makeUnexpected(std::move(user).error());
    }
    Expected<std::optional<Found>> found = find(name);
    if (!found) {
        return makeUnexpected(std::move(found).error());
    }
    if (!found->has_value()) {
        return makeError("editor.layout.not_found", "no layout is called '" + std::string(name) + "'");
    }
    std::vector<Error> warnings;
    if (Expected<EditorLayout> layout = loadFile(**found, warnings); !layout) {
        return makeUnexpected(std::move(layout).error());
    }
    if (Expected<void> set = m_config->set(kEditorLayoutSettings, EditorLayoutSettings{.defaultName = (*found)->name},
                                           m_config->layers()[*user].name);
        !set) {
        return makeUnexpected(std::move(set).error());
    }
    return (*found)->name;
}

Expected<std::string> LayoutSession::remove(std::string_view name) {
    Expected<std::size_t> user = userLayer();
    if (!user) {
        return makeUnexpected(std::move(user).error());
    }
    Expected<std::optional<Found>> found = find(name);
    if (!found) {
        return makeUnexpected(std::move(found).error());
    }
    if (!found->has_value()) {
        return makeError("editor.layout.not_found", "no layout is called '" + std::string(name) + "'");
    }
    if ((*found)->layer != *user) {
        return makeError("editor.layout.reserved_name",
                         "the layout '" + (*found)->name +
                             "' comes with the engine or the project and can't be deleted");
    }
    const std::string userName = m_config->layers()[*user].name;
    if (Expected<void> removed = m_config->removeFile(userName, layoutPath((*found)->name)); !removed) {
        return makeUnexpected(std::move(removed).error());
    }
    if (equalsIgnoreCaseAscii(defaultName(), (*found)->name)) {
        // Writing what the layers below give removes the user's choice (Config::set).
        const std::string below = defaultIn(m_config->data(kEditorLayoutSettings.file, *user));
        if (Expected<void> set =
                m_config->set(kEditorLayoutSettings, EditorLayoutSettings{.defaultName = below}, userName);
            !set) {
            return makeUnexpected(std::move(set).error());
        }
    }
    if (equalsIgnoreCaseAscii(m_name, (*found)->name)) {
        m_name.clear();
    }
    return (*found)->name;
}

LayoutLoad LayoutSession::reset() {
    std::vector<std::string> names;
    if (m_config != nullptr) {
        names.push_back(defaultName());
        names.push_back(defaultIn(m_config->data(kEditorLayoutSettings.file, m_config->layers().size() - 1)));
    }
    names.push_back(EditorLayoutSettings{}.defaultName);
    std::vector<std::string> tried;
    for (const std::string& name : names) {
        if (std::ranges::any_of(tried,
                                [&name](const std::string& other) { return equalsIgnoreCaseAscii(name, other); })) {
            continue;
        }
        tried.push_back(name);
        if (m_config == nullptr) {
            break;
        }
        Expected<std::optional<Found>> found = find(name);
        if (!found || !found->has_value()) {
            ITH_LOG_WARNING(EditorLayout, "the default layout '{}' is missing; the next default applies", name);
            continue;
        }
        std::vector<Error> warnings;
        Expected<EditorLayout> layout = loadFile(**found, warnings);
        if (!layout) {
            ITH_LOG_WARNING(EditorLayout, "the default layout is skipped: {}", layout.error().message);
            continue;
        }
        logWarnings(warnings);
        replace(std::move(*layout), (*found)->name);
        return LayoutLoad{.name = m_name, .warnings = std::move(warnings)};
    }
    // The built-in default names every P2 panel; the ones not registered yet go without warnings.
    EditorLayout builtIn = builtInDefaultLayout();
    normalizeLayout(builtIn, *m_panels, nullptr);
    replace(std::move(builtIn), {});
    return LayoutLoad{};
}

Expected<PanelOpened> LayoutSession::open(std::string_view panel) {
    readBack();
    std::string instance;
    if (m_panels->resolve(panel).has_value()) {
        instance = std::string(panel);
    } else if (const PanelDesc* desc = m_panels->find(panel); desc != nullptr) {
        for (std::uint32_t number = 1; number <= desc->maxInstances && instance.empty(); ++number) {
            std::string candidate = instanceIdOf(*desc, number);
            if (!isPanelOpen(m_current, candidate)) {
                instance = std::move(candidate);
            }
        }
        if (instance.empty()) {
            return makeError("editor.layout.no_free_instance",
                             "every instance of the panel '" + std::string(panel) + "' is open");
        }
    } else {
        return makeError("editor.layout.unknown_panel", "no panel is called '" + std::string(panel) + "'");
    }
    m_focusRequest = instance;
    if (isPanelOpen(m_current, instance)) {
        return PanelOpened{.instance = std::move(instance), .opened = false};
    }
    EditorLayout layout = m_current;
    openPanel(layout, instance, *m_panels);
    replace(std::move(layout), m_name);
    return PanelOpened{.instance = std::move(instance), .opened = true};
}

Expected<bool> LayoutSession::close(std::string_view instance) {
    if (!m_panels->resolve(instance).has_value()) {
        return makeError("editor.layout.unknown_panel", "no panel instance is called '" + std::string(instance) + "'");
    }
    readBack();
    EditorLayout layout = m_current;
    if (!closePanel(layout, instance)) {
        return false;
    }
    replace(std::move(layout), m_name);
    return true;
}

std::optional<std::string> LayoutSession::takeFocusRequest() {
    return std::exchange(m_focusRequest, std::nullopt);
}

Expected<std::vector<LayoutListing>> LayoutSession::list() {
    std::vector<LayoutListing> listings;
    if (m_config == nullptr) {
        return listings;
    }
    Expected<std::vector<ConfigFile>> files = m_config->listFiles(kLayoutFolder);
    if (!files) {
        return makeUnexpected(std::move(files).error());
    }
    for (const ConfigFile& file : *files) {
        if (file.name.ends_with(kExtension)) {
            listings.push_back(
                {.name = file.name.substr(0, file.name.size() - kExtension.size()),
                 .layer = m_config->layers()[file.layer].name,
                 .user = file.layer + 1 == m_config->layers().size() && m_config->layers()[file.layer].writable});
        }
    }
    return listings;
}

std::string LayoutSession::defaultName() {
    if (m_config == nullptr) {
        return EditorLayoutSettings{}.defaultName;
    }
    return m_config->get(kEditorLayoutSettings).defaultName;
}

} // namespace ith
