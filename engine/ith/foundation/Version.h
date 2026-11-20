#pragma once

// The engine version (ADR-0002). The numbers come from project() in the root CMakeLists.txt
// through the generated header ith/foundation/BuildInfo.h.

#include "ith/foundation/BuildInfo.h"

#include <compare>
#include <cstdint>
#include <string_view>

namespace ith {

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    friend constexpr auto operator<=>(const Version&, const Version&) = default;
};

// The version of this build of the engine, without the pre-release suffix.
inline constexpr Version kEngineVersion{
    .major = build_info::kVersionMajor, .minor = build_info::kVersionMinor, .patch = build_info::kVersionPatch};

// The full SemVer string, e.g. "0.1.0-dev". Development builds of main carry "-dev".
inline constexpr std::string_view kEngineVersionString = build_info::kVersionString;

// True for a release build of a tagged version, false for "-dev" builds of main.
inline constexpr bool kIsReleaseVersion = build_info::kVersionSuffix[0] == '\0';

} // namespace ith
