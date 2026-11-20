#include "ith/foundation/Expected.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>

namespace {

ith::Expected<int> parsePositive(int input) {
    if (input <= 0) {
        return ith::makeError("parse.not_positive", "the number must be greater than zero");
    }
    return input;
}

ith::Expected<void> validate(bool ok) {
    if (!ok) {
        return ith::makeError("validate.failed", "validation failed");
    }
    return {};
}

} // namespace

TEST_SUITE("foundation.expected") {
    TEST_CASE("holds a value") {
        const ith::Expected<int> result = parsePositive(7);
        REQUIRE(result.hasValue());
        CHECK(static_cast<bool>(result));
        CHECK(*result == 7);
        CHECK(result.value() == 7);
    }

    TEST_CASE("holds an error with code and message") {
        const ith::Expected<int> result = parsePositive(-1);
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == "parse.not_positive");
        CHECK(result.error().message == "the number must be greater than zero");
    }

    TEST_CASE("valueOr returns the value, or the fallback for an error") {
        CHECK(parsePositive(5).valueOr(0) == 5);
        CHECK(parsePositive(0).valueOr(42) == 42);
    }

    TEST_CASE("works with move-only values") {
        ith::Expected<std::unique_ptr<int>> result = std::make_unique<int>(3);
        REQUIRE(result);
        const std::unique_ptr<int> owned = std::move(result).value();
        REQUIRE(owned != nullptr);
        CHECK(*owned == 3);
    }

    TEST_CASE("the value and the error may have the same type") {
        const ith::Expected<std::string, std::string> value = std::string("value");
        const ith::Expected<std::string, std::string> error = ith::makeUnexpected(std::string("error"));
        CHECK(value.hasValue());
        CHECK(*value == "value");
        CHECK_FALSE(error.hasValue());
        CHECK(error.error() == "error");
    }

    TEST_CASE("operator-> reaches members of the value") {
        const ith::Expected<std::string> result = std::string("abc");
        CHECK(result->size() == 3);
    }

    TEST_CASE("Expected<void> reports success or an error") {
        const ith::Expected<void> ok = validate(true);
        const ith::Expected<void> failed = validate(false);
        CHECK(ok.hasValue());
        REQUIRE_FALSE(failed.hasValue());
        CHECK(failed.error().code == "validate.failed");
    }

    TEST_CASE("errors compare by code and message") {
        const ith::Error a{.code = "x", .message = "y"};
        const ith::Error b{.code = "x", .message = "y"};
        const ith::Error c{.code = "x", .message = "z"};
        CHECK(a == b);
        CHECK_FALSE(a == c);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("reading the value of an error asserts") {
        ith::test::AssertCapture capture;
        ith::Expected<int> result = parsePositive(0);
        // The read is undefined after the assert; only the assert itself is checked.
        static_cast<void>(result.operator->());
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() == "Expected holds an error, not a value");
    }

    TEST_CASE("value() of a successful Expected<void> does not assert, of a failed one does") {
        ith::test::AssertCapture capture;
        validate(true).value();
        CHECK(capture.count() == 0);
        validate(false).value();
        CHECK(capture.count() == 1);
    }
#endif
}
