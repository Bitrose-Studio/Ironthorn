// The Value tree and the JSON reader and writer (ADR-0010).

#include "ith/serialization/Json.h"

#include "ith/serialization/Document.h"
#include "ith/serialization/Value.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

// doctest prints std::string operands of failed checks through an ostream.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>

namespace {

namespace serialization = ith::serialization;
using serialization::Value;

std::string nested(std::size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

} // namespace

TEST_SUITE("serialization.value") {
    TEST_CASE("isJsonNumber accepts RFC 8259 numbers only") {
        for (const char* text : {"0", "-0", "12", "-12.5", "1e5", "1E+5", "1.25e-300", "0.0"}) {
            CAPTURE(text);
            CHECK(serialization::isJsonNumber(text));
        }
        for (const char* text : {"", "-", "+1", "01", "1.", ".5", "1e", "1e+", "0x10", "inf", "NaN", "1 ", " 1"}) {
            CAPTURE(text);
            CHECK_FALSE(serialization::isJsonNumber(text));
        }
    }

    TEST_CASE("numbers keep their text and convert exactly") {
        CHECK(Value::integer(std::numeric_limits<std::int64_t>::min()).numberText() == "-9223372036854775808");
        CHECK(Value::unsignedInteger(std::numeric_limits<std::uint64_t>::max()).numberText() == "18446744073709551615");
        CHECK(Value::real(0.1).numberText() == "0.1");
        CHECK(Value::number("1.50").numberText() == "1.50");

        const Value big = Value::number("18446744073709551615");
        CHECK(big.toNumber<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max());
        CHECK_FALSE(big.toNumber<std::int64_t>().has_value());
        CHECK_FALSE(Value::number("1.5").toNumber<std::int32_t>().has_value());
        CHECK_FALSE(Value::number("1e5").toNumber<std::int32_t>().has_value());
        CHECK(Value::number("1e5").toNumber<double>() == 100000.0);
        CHECK_FALSE(Value::number("300").toNumber<std::uint8_t>().has_value());
        CHECK_FALSE(Value::string("1").toNumber<int>().has_value());
    }

    TEST_CASE("objects keep member order; set replaces, remove removes every copy") {
        Value object = Value::object();
        object.set("b", Value::integer(1));
        object.set("a", Value::integer(2));
        object.set("b", Value::integer(3));
        REQUIRE(object.members().size() == 2);
        CHECK(object.members()[0].key == "b");
        CHECK(object.find("b")->toNumber<int>() == 3);
        CHECK(object.find("missing") == nullptr);
        CHECK(Value::integer(1).find("b") == nullptr);
        object.members().push_back(Value::Member{.key = "a", .value = Value()});
        CHECK(object.remove("a"));
        CHECK_FALSE(object.remove("a"));
        CHECK(object.members().size() == 1);
    }

    TEST_CASE("wrong-kind access and invalid numbers assert and give safe values") {
        const ith::test::AssertCapture capture;
        CHECK(Value::number("01").numberText() == "0");
        CHECK(Value::real(std::numeric_limits<double>::infinity()).numberText() == "0");
        CHECK_FALSE(Value::string("x").asBool());
        CHECK(Value().asString().empty());
        CHECK(Value().numberText().empty());
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 5);
#endif
    }
}

TEST_SUITE("serialization.json") {
    TEST_CASE("the writer prints two-space indented JSON with a newline at the end") {
        Value root = Value::object();
        root.set("name", Value::string("caf\xc3\xa9 \"quoted\"\n"));
        root.set("count", Value::number("12"));
        root.set("ratio", Value::number("0.5"));
        root.set("on", Value::boolean(true));
        root.set("none", Value());
        root.set("list", Value::array({Value::integer(1), Value::integer(2)}));
        root.set("empty", Value::object());
        const auto text = serialization::writeJson(root);
        REQUIRE(text.hasValue());
        CHECK(*text == "{\n"
                       "  \"name\": \"caf\xc3\xa9 \\\"quoted\\\"\\n\",\n"
                       "  \"count\": 12,\n"
                       "  \"ratio\": 0.5,\n"
                       "  \"on\": true,\n"
                       "  \"none\": null,\n"
                       "  \"list\": [\n"
                       "    1,\n"
                       "    2\n"
                       "  ],\n"
                       "  \"empty\": {}\n"
                       "}\n");
        const auto parsed = serialization::parseJson(*text);
        REQUIRE(parsed.hasValue());
        CHECK(*parsed == root);
    }

    TEST_CASE("the reader keeps duplicate keys and number text") {
        const auto parsed = serialization::parseJson(R"({"a": 1.50, "a": -0, "s": "x\u0000y"})");
        REQUIRE(parsed.hasValue());
        REQUIRE(parsed->members().size() == 3);
        CHECK(parsed->members()[0].value.numberText() == "1.50");
        CHECK(parsed->members()[1].value.numberText() == "-0");
        CHECK(parsed->members()[2].value.asString() == std::string("x\0y", 3));
    }

    TEST_CASE("syntax errors carry the position") {
        const auto parsed = serialization::parseJson("{\n  \"a\": 1,\n  \"b\": tru\n}", {.source = "test.json"});
        REQUIRE_FALSE(parsed.hasValue());
        CHECK(parsed.error().code == "serialization.syntax");
        CHECK(parsed.error().source == "test.json");
        CHECK(parsed.error().line == 3);
        CHECK(parsed.error().offset.has_value());
        CHECK(parsed.error().toString().rfind("test.json:3:", 0) == 0);

        for (const char* text : {"", "   ", "{", "[1,]", "{\"a\":1,}", "01", "NaN", "Infinity", "'a'", "{a:1}",
                                 "[1] [2]", "\"\xff\"", "\xef\xbb\xbf{}"}) {
            CAPTURE(text);
            const auto result = serialization::parseJson(text);
            CHECK_FALSE(result.hasValue());
        }
    }

    TEST_CASE("comments are accepted only in config syntax") {
        const std::string text = "{\n  // the window\n  \"width\": 1920 /* pixels */\n}";
        const auto strict = serialization::parseJson(text);
        REQUIRE_FALSE(strict.hasValue());
        CHECK(strict.error().code == "serialization.syntax");
        CHECK(strict.error().line == 2);
        const auto config = serialization::parseJson(text, {.syntax = serialization::JsonSyntax::Config});
        REQUIRE(config.hasValue());
        CHECK(config->find("width")->toNumber<int>() == 1920);
    }

    TEST_CASE("nesting is limited to kMaxDepth in reading and writing") {
        CHECK(serialization::parseJson(nested(serialization::kMaxDepth)).hasValue());
        const auto deep = serialization::parseJson(nested(serialization::kMaxDepth + 1));
        REQUIRE_FALSE(deep.hasValue());
        CHECK(deep.error().code == "serialization.too_deep");
        const auto veryDeep = serialization::parseJson(nested(100000));
        REQUIRE_FALSE(veryDeep.hasValue());
        CHECK(veryDeep.error().code == "serialization.too_deep");

        Value value = Value::array();
        for (std::size_t depth = 1; depth < serialization::kMaxDepth; ++depth) {
            value = Value::array({value});
        }
        CHECK(serialization::writeJson(value).hasValue());
        value = Value::array({value});
        const auto written = serialization::writeJson(value);
        REQUIRE_FALSE(written.hasValue());
        CHECK(written.error().code == "serialization.too_deep");
    }

    TEST_CASE("documents larger than kMaxTextSize are refused before parsing") {
        const std::string huge(serialization::kMaxTextSize + 1, ' ');
        const auto parsed = serialization::parseJson(huge);
        REQUIRE_FALSE(parsed.hasValue());
        CHECK(parsed.error().code == "serialization.too_large");
    }

    TEST_CASE("strings that are not UTF-8 are not written") {
        const auto written = serialization::writeJson(Value::string("\xff\xfe"));
        REQUIRE_FALSE(written.hasValue());
        CHECK(written.error().code == "serialization.write_failed");
    }
}
