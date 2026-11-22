#pragma once

// The name of the level document format (ADR-0024, ADR-0034), on its own so that tools that
// only recognise level files, such as the asset database's scan, need not include the ECS.

#include <string_view>

namespace ith {

// The document format of levels: the only format an ".ithmap" file holds, and one an ".ithasset"
// file never holds.
inline constexpr std::string_view kLevelFormat = "ith.level";

} // namespace ith
