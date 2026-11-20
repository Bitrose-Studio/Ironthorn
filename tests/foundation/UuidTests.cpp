#include "ith/foundation/Uuid.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

bool isVersion4RfcVariant(const ith::Uuid& id) {
    return ((id.high() >> 12) & 0xfu) == 4 && (id.low() >> 62) == 0b10u;
}

void checkMalformed(std::string_view text) {
    const ith::Expected<ith::Uuid> parsed = ith::Uuid::parse(text);
    REQUIRE_FALSE(parsed.hasValue());
    CHECK(parsed.error().code == "uuid.malformed");
    CHECK_FALSE(parsed.error().message.empty());
}

} // namespace

TEST_SUITE("foundation.uuid") {
    TEST_CASE("the default Uuid is nil") {
        constexpr ith::Uuid id;
        static_assert(id.isNil());
        CHECK(id.toString() == "00000000-0000-0000-0000-000000000000");
        CHECK(id == ith::Uuid(0, 0));
        CHECK_FALSE(ith::Uuid(0, 1).isNil());
        CHECK_FALSE(ith::Uuid(1, 0).isNil());
    }

    TEST_CASE("generate gives version 4, RFC variant identifiers") {
        for (int index = 0; index < 100; ++index) {
            const ith::Uuid id = ith::Uuid::generate();
            CHECK_FALSE(id.isNil());
            CHECK(isVersion4RfcVariant(id));

            const std::string text = id.toString();
            CHECK(text.size() == 36);
            CHECK(text[14] == '4');
            const char variant = text[19];
            CHECK((variant == '8' || variant == '9' || variant == 'a' || variant == 'b'));
        }
    }

    TEST_CASE("generated identifiers do not collide") {
        std::unordered_set<ith::Uuid> seen;
        for (int index = 0; index < 20000; ++index) {
            CHECK(seen.insert(ith::Uuid::generate()).second);
        }
    }

    TEST_CASE("generate is safe to call from several threads, and threads get different values") {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 2000;
        std::vector<std::vector<ith::Uuid>> results(kThreads);
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([&results, thread] {
                std::vector<ith::Uuid>& list = results[static_cast<std::size_t>(thread)];
                list.reserve(kPerThread);
                for (int index = 0; index < kPerThread; ++index) {
                    list.push_back(ith::Uuid::generate());
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }

        std::set<ith::Uuid> all;
        for (const std::vector<ith::Uuid>& list : results) {
            all.insert(list.begin(), list.end());
        }
        CHECK(all.size() == static_cast<std::size_t>(kThreads * kPerThread));
    }

    TEST_CASE("toString and parse round-trip") {
        const ith::Uuid original(0x0123456789abcdefull, 0x8edcba9876543210ull);
        const std::string text = original.toString();
        CHECK(text == "01234567-89ab-cdef-8edc-ba9876543210");

        const ith::Expected<ith::Uuid> parsed = ith::Uuid::parse(text);
        REQUIRE(parsed.hasValue());
        CHECK(*parsed == original);
        CHECK(parsed->high() == 0x0123456789abcdefull);
        CHECK(parsed->low() == 0x8edcba9876543210ull);
    }

    TEST_CASE("toString and parse round-trip the extreme values") {
        const ith::Uuid max(~0ull, ~0ull);
        CHECK(max.toString() == "ffffffff-ffff-ffff-ffff-ffffffffffff");
        CHECK(*ith::Uuid::parse(max.toString()) == max);

        const ith::Uuid generated = ith::Uuid::generate();
        CHECK(*ith::Uuid::parse(generated.toString()) == generated);
    }

    TEST_CASE("parse accepts uppercase and mixed case") {
        const ith::Expected<ith::Uuid> upper = ith::Uuid::parse("01234567-89AB-CDEF-8EDC-BA9876543210");
        REQUIRE(upper.hasValue());
        CHECK(upper->toString() == "01234567-89ab-cdef-8edc-ba9876543210");

        const ith::Expected<ith::Uuid> mixed = ith::Uuid::parse("01234567-89aB-CdEf-8eDc-bA9876543210");
        REQUIRE(mixed.hasValue());
        CHECK(*mixed == *upper);
    }

    TEST_CASE("parse rejects malformed text") {
        checkMalformed("");
        checkMalformed("01234567-89ab-cdef-8edc-ba987654321");
        checkMalformed("01234567-89ab-cdef-8edc-ba98765432100");
        checkMalformed("0123456789abcdef8edcba9876543210");
        checkMalformed("01234567_89ab_cdef_8edc_ba9876543210");
        checkMalformed("0123456g-89ab-cdef-8edc-ba9876543210");
        checkMalformed("01234567-89ab-cdef-8edc-ba987654321-");
        checkMalformed("-01234567-89ab-cdef-8edc-ba987654321");
        checkMalformed("{1234567-89ab-cdef-8edc-ba987654321}");
        checkMalformed(" 1234567-89ab-cdef-8edc-ba9876543210");
        checkMalformed("0123456789-ab-cdef-8edc-ba9876543210");
        checkMalformed(std::string_view("01234567-89ab-cdef-8edc-ba987654321\0", 36));
    }

    TEST_CASE("Uuids order like their text forms and hash consistently") {
        const ith::Uuid low(1, 5);
        const ith::Uuid high(2, 0);
        CHECK(low < high);
        CHECK(low.toString() < high.toString());
        CHECK(ith::Uuid(1, 4) < low);
        CHECK(std::hash<ith::Uuid>()(low) == std::hash<ith::Uuid>()(ith::Uuid(1, 5)));
    }
}
