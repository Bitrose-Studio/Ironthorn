#pragma once

// Content paths (spec section 2): relative to the project's Content folder, UTF-8, '/'-separated;
// the content root is the empty path "". The helpers the Content Browser's model, its commands and
// the session share. Internal to editor_core.

#include "ith/file_system/FileSystem.h"

#include <optional>
#include <string>
#include <string_view>

namespace ith::detail {

// The folder that holds `path`: "" for an item at the root (and for the root itself).
[[nodiscard]] inline std::string contentParent(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

// The last name of `path`; the whole path for an item at the root, "" for the root.
[[nodiscard]] inline std::string_view contentName(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// `name` in the folder `folder`; just `name` in the root.
[[nodiscard]] inline std::string contentChild(std::string_view folder, std::string_view name) {
    return folder.empty() ? std::string(name) : std::string(folder) + "/" + std::string(name);
}

// Whether `path` is the folder `folder` or below it, by bytes. The root ("") holds everything.
[[nodiscard]] inline bool isInContentFolder(std::string_view path, std::string_view folder) {
    if (folder.empty() || path == folder) {
        return true;
    }
    return path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/';
}

// Whether `path` is `ancestor` or below it, for an item that can move or be deleted: never true for
// the root as `ancestor`, which is no such item.
[[nodiscard]] inline bool isAtOrBelow(std::string_view path, std::string_view ancestor) {
    return !ancestor.empty() && isInContentFolder(path, ancestor);
}

// The file system path `fullPath` as the content path of a file or folder inside the Content folder
// `contentDirectory`, or nullopt when it is not inside it or is that folder itself. Compared as the
// operating system sees it (fs::pathInside: another case or a short name on Windows, links), and
// spelled as the disk spells the part that exists. Costs a path resolution on disk.
[[nodiscard]] inline std::optional<std::string> contentFileOf(std::string_view contentDirectory,
                                                              std::string_view fullPath) {
    std::optional<std::string> path = fs::pathInside(fullPath, contentDirectory);
    return path.has_value() && !path->empty() ? path : std::nullopt;
}

} // namespace ith::detail
