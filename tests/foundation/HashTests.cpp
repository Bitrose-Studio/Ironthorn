#include "ith/foundation/Hash.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace {

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

} // namespace

TEST_SUITE("foundation.hash") {
    TEST_CASE("fnv1a64 matches the published test vectors") {
        static_assert(ith::fnv1a64("") == 0xcbf29ce484222325ull);
        static_assert(ith::fnv1a64("") == ith::kFnv1a64Basis);
        static_assert(ith::fnv1a64("a") == 0xaf63dc4c8601ec8cull);
        static_assert(ith::fnv1a64("foobar") == 0x85944171f73967e8ull);
        CHECK(ith::fnv1a64("foobar") == 0x85944171f73967e8ull);
    }

    TEST_CASE("fnv1a64 over bytes equals fnv1a64 over the same text") {
        CHECK(ith::fnv1a64(bytesOf("foobar")) == 0x85944171f73967e8ull);
        CHECK(ith::fnv1a64(bytesOf("")) == ith::kFnv1a64Basis);
        CHECK(ith::fnv1a64(bytesOf("\xff\x80")) == ith::fnv1a64("\xff\x80"));
    }

    TEST_CASE("fnv1a64 hashes embedded NUL bytes") {
        const std::string withNul("a\0b", 3);
        CHECK(ith::fnv1a64(withNul) != ith::fnv1a64("ab"));
        CHECK(ith::fnv1a64(std::string_view("\0", 1)) != ith::fnv1a64(""));
    }

    TEST_CASE("fnv1a64 continues a hash through the basis") {
        static_assert(ith::fnv1a64("foobar") == ith::fnv1a64("bar", ith::fnv1a64("foo")));
        CHECK(ith::fnv1a64(bytesOf("bar"), ith::fnv1a64("foo")) == ith::fnv1a64("foobar"));
    }

    TEST_CASE("fnv1a64 is case-sensitive") {
        CHECK(ith::fnv1a64("render") != ith::fnv1a64("Render"));
    }

    TEST_CASE("fnv1a64 spreads sequential keys") {
        std::set<std::uint64_t> seen;
        for (int index = 0; index < 10000; ++index) {
            seen.insert(ith::fnv1a64("key" + std::to_string(index)));
        }
        CHECK(seen.size() == 10000);
    }

    TEST_CASE("hashCombine depends on order and on every input") {
        static_assert(ith::hashCombine(0, 1) == ith::hashCombine(0, 1));
        const std::uint64_t ab = ith::hashCombine(ith::hashCombine(0, 1), 2);
        const std::uint64_t ba = ith::hashCombine(ith::hashCombine(0, 2), 1);
        CHECK(ab != ba);
        CHECK(ith::hashCombine(0, 1) != ith::hashCombine(0, 2));
        CHECK(ith::hashCombine(1, 0) != ith::hashCombine(2, 0));
    }

    TEST_CASE("hashCombine has a fixed value, so stored keys stay valid") {
        static_assert(ith::hashCombine(0, 0) == 0x9e3779b97f4a7c15ull);
        static_assert(ith::hashCombine(0x1234, 7) == 0x9e3779b9806daf0bull);
    }
}
