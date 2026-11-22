#pragma once

// Pieces the Windows and the POSIX implementation share. Internal to the module.

#include "ith/file_system/FileSystem.h"
#include "ith/foundation/Expected.h"

#include <string>
#include <string_view>
#include <vector>

namespace ith::fs::detail {

// True when the calling thread's test fault is `fault` (always false in Shipping builds).
[[nodiscard]] bool faultAt(int fault) noexcept;

// The fault points of writeFileAtomic(), as ints so Shipping builds need no testing enum.
inline constexpr int kFaultAfterTemporaryWrite = 1;
inline constexpr int kFaultAfterFlush = 2;
inline constexpr int kFaultBeforeRename = 3;

// The "fs.io" error a test fault produces.
[[nodiscard]] Unexpected<Error> injectedFault(std::string_view path);

// The error "<what> '<path>': <osText>" with `code`.
[[nodiscard]] Unexpected<Error> pathError(std::string_view code, std::string_view what, std::string_view path,
                                          std::string_view osText);

// Checks that `path` is usable (valid UTF-8, no NUL byte, not empty) and returns it normalized.
// Fails with "fs.invalid_path".
[[nodiscard]] Expected<std::string> checkedPath(std::string_view path);

// `path` (normalized) with the name writeFileAtomic() gives its temporary file.
[[nodiscard]] std::string temporaryPathFor(std::string_view path);

// `text` with every byte sequence that isn't UTF-8 replaced by U+FFFD.
[[nodiscard]] std::string lossyUtf8(std::string_view text);

// Creates one directory. Fails with "fs.already_exists" when something is there.
[[nodiscard]] Expected<void> makeDirectory(std::string_view path);

// createDirectories(path), adding the directories it created to `created`, outermost first.
[[nodiscard]] Expected<void> createDirectoriesTracked(std::string_view path, std::vector<std::string>& created);

} // namespace ith::fs::detail
