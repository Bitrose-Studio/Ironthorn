#include "ith/foundation/StringUtils.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

// doctest prints std::string_view operands of failed checks through an ostream.
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

using Parts = std::vector<std::string_view>;

TEST_SUITE("foundation.strings") {
    TEST_CASE("trim removes surrounding ASCII whitespace only") {
        CHECK(ith::trim("  hello world \t\r\n") == "hello world");
        CHECK(ith::trim("\v\fx\f\v") == "x");
        CHECK(ith::trim("hello") == "hello");
        CHECK(ith::trim(" a b ") == "a b");
        CHECK(ith::trim("   ").empty());
        CHECK(ith::trim("").empty());
    }

    TEST_CASE("trim keeps non-ASCII bytes, including UTF-8 spaces") {
        // U+00A0 (no-break space) is not ASCII whitespace and stays.
        CHECK(ith::trim("\xc2\xa0x ") == "\xc2\xa0x");
    }

    TEST_CASE("trim returns a view into its argument") {
        const std::string_view text = "  abc  ";
        const std::string_view trimmed = ith::trim(text);
        CHECK(trimmed.data() == text.data() + 2);
    }

    TEST_CASE("split keeps empty parts") {
        CHECK(ith::split("a,b,c", ',') == Parts{"a", "b", "c"});
        CHECK(ith::split("a,,b", ',') == Parts{"a", "", "b"});
        CHECK(ith::split(",a,", ',') == Parts{"", "a", ""});
        CHECK(ith::split(",", ',') == Parts{"", ""});
        CHECK(ith::split("", ',') == Parts{""});
        CHECK(ith::split("no-delimiter", ',') == Parts{"no-delimiter"});
    }

    TEST_CASE("split can use a NUL delimiter") {
        const std::string text("a\0b", 3);
        CHECK(ith::split(text, '\0') == Parts{"a", "b"});
    }

    TEST_CASE("toLowerAscii lowercases ASCII letters and leaves other bytes alone") {
        CHECK(ith::toLowerAscii("Hello WORLD 123 @[`{") == "hello world 123 @[`{");
        CHECK(ith::toLowerAscii("\xc3\x89T\xc3\x89") == "\xc3\x89t\xc3\x89");
        CHECK(ith::toLowerAscii("").empty());
    }

    TEST_CASE("equalsIgnoreCaseAscii compares ASCII letters case-insensitively") {
        CHECK(ith::equalsIgnoreCaseAscii("True", "tRUE"));
        CHECK(ith::equalsIgnoreCaseAscii("", ""));
        CHECK_FALSE(ith::equalsIgnoreCaseAscii("true", "true "));
        CHECK_FALSE(ith::equalsIgnoreCaseAscii("abc", "abd"));
        CHECK_FALSE(ith::equalsIgnoreCaseAscii("", "a"));
        // '@' and '`' sit next to the letter ranges and must not match each other.
        CHECK_FALSE(ith::equalsIgnoreCaseAscii("@", "`"));
        // Non-ASCII bytes compare exactly.
        CHECK_FALSE(ith::equalsIgnoreCaseAscii("\xc3\x89", "\xc3\xa9"));
    }

    TEST_CASE("replaceAll replaces every non-overlapping occurrence") {
        CHECK(ith::replaceAll("a.b.c", ".", "/") == "a/b/c");
        CHECK(ith::replaceAll("aaaa", "aa", "b") == "bb");
        CHECK(ith::replaceAll("aaa", "aa", "b") == "ba");
        CHECK(ith::replaceAll("hello", "xyz", "!") == "hello");
        CHECK(ith::replaceAll("x", "x", "xx") == "xx");
        CHECK(ith::replaceAll("", "a", "b").empty());
        CHECK(ith::replaceAll("a-b", "-", "") == "ab");
        CHECK(ith::replaceAll("ab", "abc", "x") == "ab");
    }

    TEST_CASE("hexDigitValue accepts both letter cases and rejects everything else") {
        static_assert(ith::hexDigitValue('0') == 0 && ith::hexDigitValue('9') == 9);
        static_assert(ith::hexDigitValue('a') == 10 && ith::hexDigitValue('f') == 15);
        static_assert(ith::hexDigitValue('A') == 10 && ith::hexDigitValue('F') == 15);

        // The characters immediately outside each accepted range, and a few others.
        for (const char character : {'/', ':', '@', 'G', '`', 'g', ' ', '\0', '-', '\x80', '\xff'}) {
            CHECK(ith::hexDigitValue(character) == -1);
        }
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("replaceAll with an empty pattern asserts and returns the text unchanged") {
        ith::test::AssertCapture capture;
        const std::string result = ith::replaceAll("text", "", "x");
        CHECK(capture.count() == 1);
        CHECK(result == "text");
    }
#endif
}
