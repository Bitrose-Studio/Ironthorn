#pragma once

// Lexical path helpers (ADR-0035 rule 1). They only look at the text: no file system access, the
// same results on every platform, and no case folding.
//
// Paths are UTF-8 with '/' separators. Three kinds of root are recognized on every platform:
// "/" (POSIX), a drive root "C:/" and a UNC root "//server/share". A path with a root is absolute.
//
//   ith::fs::normalizePath("C:\\Game\\.\\Content\\..\\Config\\"); // "C:/Game/Config"
//   ith::fs::joinPath("/home/ann", "Saved/Editor");             // "/home/ann/Saved/Editor"
//   ith::fs::parentPath("Content/Ships/Hull.gltf");              // "Content/Ships"
//   ith::fs::fileName("Content/Ships/Hull.gltf");                // "Hull.gltf"
//   ith::fs::extension("Content/Ships/Hull.gltf");               // ".gltf"
//
// All functions are thread-safe: they read only their arguments.

#include "ith/foundation/Compiler.h"

#include <string>
#include <string_view>

namespace ith::fs {

// The length of the root at the start of `path`, 0 when it has none: 1 for "/", 3 for "C:/"
// (either slash), and the length of "//server/share" for a UNC root (either slash; the share may
// be missing in a malformed path). A drive letter without a slash ("C:x") is not a root.
[[nodiscard]] std::size_t rootLength(std::string_view path) noexcept;

// True when `path` starts with a root (see rootLength).
[[nodiscard]] bool isAbsolutePath(std::string_view path) noexcept;

// `path` with '\' turned into '/', repeated '/' collapsed (except the two that start a UNC root),
// "." segments removed, ".." segments resolved against the segment before them, and no trailing
// '/' (except a root's own). A ".." directly below a root is dropped: a path never goes above its
// root. Leading ".." segments of a relative path are kept. A relative path that resolves to
// nothing is ".", and the empty path stays empty. Normalizing twice changes nothing.
[[nodiscard]] std::string normalizePath(std::string_view path);

// `relative` appended to `base` with one '/', normalized; `relative` itself (normalized) when it
// is absolute, and `base` (normalized) when it is empty.
[[nodiscard]] std::string joinPath(std::string_view base, std::string_view relative);

// The path without its last segment, for a normalized `path`: "a/b" gives "a", "a" gives "",
// "/a" gives "/", "C:/a" gives "C:/", and a root gives itself. The view points into `path`.
[[nodiscard]] std::string_view parentPath(std::string_view path ITH_LIFETIMEBOUND) noexcept;

// The last segment of a normalized `path`, empty for a root. The view points into `path`.
[[nodiscard]] std::string_view fileName(std::string_view path ITH_LIFETIMEBOUND) noexcept;

// The extension of fileName(path) with its dot, e.g. ".gltf" (the last dot: "a.tar.gz" gives
// ".gz"); empty when the name has no dot or its only dot comes first (".gitignore"). The view
// points into `path`.
[[nodiscard]] std::string_view extension(std::string_view path ITH_LIFETIMEBOUND) noexcept;

} // namespace ith::fs
