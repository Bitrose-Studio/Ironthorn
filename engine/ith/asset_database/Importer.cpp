#include "ith/asset_database/Importer.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/StringUtils.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace ith {
namespace {

bool isTypeCharacter(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
}

bool isLowercaseExtension(std::string_view extension) noexcept {
    return extension.size() > 1 && extension.front() == '.' && toLowerAscii(extension) == extension;
}

// The extension of the file name in `path`, lowercased: ".png" for "Textures/Rock.PNG", empty
// when the name has no dot or only a leading one.
std::string lowercaseExtension(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return toLowerAscii(name.substr(dot));
}

std::string escapeName(std::string_view name) {
    std::string escaped;
    escaped.reserve(name.size());
    for (const char character : name) {
        if (character == '%') {
            escaped += "%25";
        } else if (character == '#') {
            escaped += "%23";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

} // namespace

void ImporterRegistry::add(std::unique_ptr<Importer> importer) {
    ITH_ASSERT_MSG(importer != nullptr, "ImporterRegistry::add: no importer");
    if (importer == nullptr) {
        return;
    }
    ITH_ASSERT_MSG(!importer->id().empty(), "ImporterRegistry::add: the importer id is empty");
    ITH_ASSERT_MSG(find(importer->id()) == nullptr, "ImporterRegistry::add: the importer id is already registered");
    ITH_ASSERT_MSG(!importer->extensions().empty(), "ImporterRegistry::add: the importer has no extensions");
    for (const std::string_view extension : importer->extensions()) {
        // Evaluated outside the asserts, which compile to nothing without ITH_ENABLE_ASSERTS.
        [[maybe_unused]] const bool lowercase = isLowercaseExtension(extension);
        [[maybe_unused]] const bool unique = findForExtension(extension) == nullptr;
        ITH_ASSERT_MSG(lowercase, "ImporterRegistry::add: extensions are lowercase and start with '.'");
        ITH_ASSERT_MSG(unique, "ImporterRegistry::add: the extension is already registered");
    }
    m_importers.push_back(std::move(importer));
}

const Importer* ImporterRegistry::find(std::string_view id) const noexcept {
    for (const std::unique_ptr<Importer>& importer : m_importers) {
        if (importer->id() == id) {
            return importer.get();
        }
    }
    return nullptr;
}

const Importer* ImporterRegistry::findForPath(std::string_view path) const {
    const std::string extension = lowercaseExtension(path);
    if (extension.empty()) {
        return nullptr;
    }
    return findForExtension(extension);
}

std::vector<std::string_view> ImporterRegistry::extensions() const {
    std::vector<std::string_view> result;
    for (const std::unique_ptr<Importer>& importer : m_importers) {
        const std::span<const std::string_view> own = importer->extensions();
        result.insert(result.end(), own.begin(), own.end());
    }
    return result;
}

const Importer* ImporterRegistry::findForExtension(std::string_view extension) const noexcept {
    for (const std::unique_ptr<Importer>& importer : m_importers) {
        const std::span<const std::string_view> extensions = importer->extensions();
        if (std::ranges::find(extensions, extension) != extensions.end()) {
            return importer.get();
        }
    }
    return nullptr;
}

Expected<std::vector<std::string>> sourceKeys(std::span<const ImportedObject> objects) {
    std::vector<std::string> keys;
    keys.reserve(objects.size());
    // How often each named key, and each type's unnamed objects, have been seen so far.
    std::unordered_map<std::string, std::size_t> seenNames;
    std::unordered_map<std::string, std::size_t> seenUnnamed;
    for (const ImportedObject& object : objects) {
        if (object.type.empty() || !std::ranges::all_of(object.type, isTypeCharacter)) {
            return makeError("import.invalid_output", "an imported object has the type '" + object.type +
                                                          "'; types are lowercase ASCII letters, digits and '_'");
        }
        if (object.name.empty()) {
            const std::size_t index = seenUnnamed[object.type]++;
            keys.push_back(object.type + ":#" + std::to_string(index));
            continue;
        }
        std::string key = object.type + ":" + escapeName(object.name);
        const std::size_t occurrence = seenNames[key]++;
        if (occurrence > 0) {
            key += "#" + std::to_string(occurrence);
        }
        keys.push_back(std::move(key));
    }
    return keys;
}

} // namespace ith
