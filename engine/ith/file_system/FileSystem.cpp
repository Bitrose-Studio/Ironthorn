#include "ith/file_system/FileSystem.h"

#include "ith/file_system/FileSystemInternal.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/StringUtils.h"
#include "ith/foundation/Uuid.h"

#include <ranges>

namespace ith::fs {
namespace {

#ifndef ITH_SHIPPING
thread_local testing::WriteFault g_writeFault = testing::WriteFault::None;
#endif

// The length of the UTF-8 sequence that starts `text` when it is well formed, else 0.
std::size_t validSequenceLength(std::string_view text) noexcept {
    const auto lead = static_cast<unsigned char>(text[0]);
    std::size_t length = 0;
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
        length = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        length = 3;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        length = 4;
    } else {
        return 0;
    }
    if (text.size() < length) {
        return 0;
    }
    return isValidUtf8(text.substr(0, length)) ? length : 0;
}

} // namespace

bool isTemporaryFileName(std::string_view path) noexcept {
    constexpr std::size_t kUuidLength = 36;
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t marker = kTemporaryMarker.size();
    return name.size() > marker + kUuidLength &&
           name.substr(name.size() - kUuidLength - marker, marker) == kTemporaryMarker &&
           Uuid::parse(name.substr(name.size() - kUuidLength)).hasValue();
}

Expected<void> createDirectories(std::string_view path) {
    std::vector<std::string> created;
    return detail::createDirectoriesTracked(path, created);
}

#ifndef ITH_SHIPPING
void testing::setWriteFault(WriteFault fault) noexcept {
    g_writeFault = fault;
}
#endif

namespace detail {

bool faultAt(int fault) noexcept {
#ifndef ITH_SHIPPING
    return static_cast<int>(g_writeFault) == fault;
#else
    static_cast<void>(fault);
    return false;
#endif
}

Unexpected<Error> injectedFault(std::string_view path) {
    return pathError("fs.io", "can't write", path, "a test fault was injected");
}

Unexpected<Error> pathError(std::string_view code, std::string_view what, std::string_view path,
                            std::string_view osText) {
    std::string message(what);
    message += " '";
    message += path;
    message += '\'';
    if (!osText.empty()) {
        message += ": ";
        message += osText;
    }
    return makeError(std::string(code), std::move(message));
}

Expected<std::string> checkedPath(std::string_view path) {
    if (path.empty()) {
        return makeError("fs.invalid_path", "the path is empty");
    }
    if (!isValidUtf8(path) || path.find('\0') != std::string_view::npos) {
        return makeError("fs.invalid_path",
                         "'" + lossyUtf8(path) + "' is not a valid path: it must be UTF-8 without NUL");
    }
    return normalizePath(path);
}

std::string temporaryPathFor(std::string_view path) {
    std::string temporary(path);
    temporary += kTemporaryMarker;
    temporary += Uuid::generate().toString();
    return temporary;
}

std::string lossyUtf8(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    while (!text.empty()) {
        const std::size_t length = validSequenceLength(text);
        if (length == 0) {
            result += "\xef\xbf\xbd";
            text.remove_prefix(1);
            continue;
        }
        result += text.substr(0, length);
        text.remove_prefix(length);
    }
    return result;
}

Expected<void> createDirectoriesTracked(std::string_view path, std::vector<std::string>& created) {
    Expected<std::string> normalized = checkedPath(path);
    if (!normalized) {
        return makeUnexpected(std::move(normalized).error());
    }
    const auto isDirectory = [](const FileInfo& info) {
        return info.kind == FileKind::Directory || info.kind == FileKind::DirectoryLink;
    };
    // Walk up to the first directory that exists, then create the missing ones top down.
    std::vector<std::string_view> missing;
    std::string_view current = *normalized;
    while (!current.empty()) {
        Expected<std::optional<FileInfo>> info = fileInfo(current);
        if (!info) {
            return makeUnexpected(std::move(info).error());
        }
        if (info->has_value()) {
            if (!isDirectory(**info)) {
                return pathError("fs.not_a_directory", "can't create the directory", path,
                                 "'" + std::string(current) + "' is not a directory");
            }
            break;
        }
        missing.push_back(current);
        const std::string_view parent = parentPath(current);
        if (parent == current) {
            break;
        }
        current = parent;
    }
    for (const std::string_view directory : std::views::reverse(missing)) {
        Expected<void> made = makeDirectory(directory);
        if (made) {
            created.emplace_back(directory);
            continue;
        }
        if (made.error().code != "fs.already_exists") {
            return made;
        }
        // Another thread or process created it first; fine when it is a directory.
        Expected<std::optional<FileInfo>> info = fileInfo(directory);
        if (!info || !info->has_value() || !isDirectory(**info)) {
            return pathError("fs.not_a_directory", "can't create the directory", path,
                             "'" + std::string(directory) + "' is not a directory");
        }
    }
    return {};
}

} // namespace detail
} // namespace ith::fs
