#pragma once

// File helpers shared by the asset database and the derived-data cache. Internal to the module.
//
// Paths cross the API as UTF-8 strings with '/' separators; these helpers convert them to and
// from std::filesystem::path. Every std::filesystem call uses its std::error_code overload.
// All functions are thread-safe for distinct files.

#include "ith/foundation/Expected.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ith::detail::asset_files {

// The marker in the names of the temporary files writeFileAtomically() writes next to its target:
// "<name>.tmp-<uuid>". One left behind by a crash is reported by AssetDatabase::scan().
inline constexpr std::string_view kTemporaryMarker = ".tmp-";

// The path as UTF-8 with '/' separators. Precondition: the path is valid Unicode (it was built
// from valid UTF-8, or toUtf8Checked accepted it); on Windows the conversion ends the process
// otherwise.
[[nodiscard]] std::string toUtf8(const std::filesystem::path& path);

// The path as UTF-8 with '/' separators, or nullopt when it isn't valid Unicode: a name with an
// unpaired surrogate on Windows, a name that isn't UTF-8 elsewhere. Use it for names read from
// disk.
[[nodiscard]] std::optional<std::string> toUtf8Checked(const std::filesystem::path& path);

// A path from UTF-8 text. Precondition: isValidUtf8(text); on Windows the conversion ends the
// process otherwise.
[[nodiscard]] std::filesystem::path fromUtf8(std::string_view text);

// The whole file as bytes. Fails with "asset.too_large" when the file (or the file a symbolic
// link points to) is larger than `maxSize`, checked before anything is allocated, or with
// "asset.io" when it can't be opened or read.
[[nodiscard]] Expected<std::string> readFile(const std::filesystem::path& path, std::size_t maxSize);

// Writes `bytes` to a temporary file next to `path` and renames it over `path`, so readers see
// either the old or the new file, never a partial one. Creates missing parent directories.
// Fails with "asset.io"; the temporary file is removed on failure. The data is not flushed to the
// disk, so a power loss can still lose the latest write.
[[nodiscard]] Expected<void> writeFileAtomically(const std::filesystem::path& path, std::string_view bytes);

// The "asset.io" error for a failed file system call on `path`.
[[nodiscard]] Unexpected<Error> ioError(std::string_view what, const std::filesystem::path& path,
                                        const std::error_code& error);

} // namespace ith::detail::asset_files
