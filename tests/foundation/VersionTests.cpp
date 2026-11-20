#include "ith/foundation/Version.h"

#include <doctest/doctest.h>

#include <string>

TEST_SUITE("foundation.version") {
    TEST_CASE("the version string starts with major.minor.patch") {
        const std::string expected = std::to_string(ith::kEngineVersion.major) + "." +
                                     std::to_string(ith::kEngineVersion.minor) + "." +
                                     std::to_string(ith::kEngineVersion.patch);
        const std::string actual(ith::kEngineVersionString);
        CHECK(actual.rfind(expected, 0) == 0);
    }

    TEST_CASE("a release version has no suffix, a development version ends in -dev") {
        const std::string actual(ith::kEngineVersionString);
        if (ith::kIsReleaseVersion) {
            CHECK(actual.find('-') == std::string::npos);
        } else {
            CHECK(actual.ends_with("-dev"));
        }
    }

    TEST_CASE("versions compare by major, then minor, then patch") {
        constexpr ith::Version v010{.major = 0, .minor = 1, .patch = 0};
        constexpr ith::Version v011{.major = 0, .minor = 1, .patch = 1};
        constexpr ith::Version v100{.major = 1, .minor = 0, .patch = 0};
        static_assert(v010 < v011);
        static_assert(v011 < v100);
        static_assert(v100 == ith::Version{.major = 1, .minor = 0, .patch = 0});
        CHECK(v010 < v100);
    }
}
