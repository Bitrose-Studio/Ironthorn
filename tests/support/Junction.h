#pragma once

// A Windows junction (a mount point reparse point) for tests of code that must see through it. Made
// with FSCTL_SET_REPARSE_POINT, which needs no privilege, unlike a symbolic link. Not a test of the
// engine's own file functions, so it calls Win32 directly.

#include <string>

namespace ith::test {

// Creates the folder `link` (absolute, UTF-8, '/' or '\') as a junction to the folder `target`
// (absolute). False when it can't (not Windows, a file system without reparse points); `link` is
// then left as an empty folder or not made.
[[nodiscard]] bool makeJunction(const std::string& link, const std::string& target);

} // namespace ith::test
