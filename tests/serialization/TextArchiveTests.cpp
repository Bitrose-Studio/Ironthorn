// The text archive: document layout, exact numbers, strict reading with paths, unknown fields,
// FormerName, versions and migrations, and validate-then-apply (ADR-0010, ADR-0018 rule 10).

#include "ith/serialization/TextArchive.h"

#include "ArchiveTypes.h"
#include "EditorArchiveTypes.h"
#include "Loadout.h"
#include "ReflectedTypes.h"

#include <doctest/doctest.h>

// doctest prints std::string operands of failed checks through an ostream.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace serialization = ith::serialization;
using serialization::Error;
using serialization::Value;

constexpr serialization::DocumentFormat kFormat{.name = "test.text", .version = 1};

std::string document(const std::string& data, const std::string& version = "1") {
    return R"({"format": "test.text", "version": )" + version + R"(, "data": )" + data + "}";
}

// Reads `data` as a T; the error, or a default Error when the read succeeds.
template <typename T>
Error readError(const std::string& data,
                serialization::UnknownFields unknownFields = serialization::UnknownFields::Warn) {
    T value{};
    const auto result = serialization::readText(document(data), kFormat, value, {.unknownFields = unknownFields});
    return result.hasValue() ? Error{} : result.error();
}

template <typename T>
void checkTextRoundTrip(T value) {
    const auto text = serialization::writeText(kFormat, value);
    REQUIRE_MESSAGE(text.hasValue(), text.error().toString());
    T read{};
    const auto result = serialization::readText(*text, kFormat, read);
    REQUIRE_MESSAGE(result.hasValue(), result.error().toString());
    if constexpr (sizeof(T) == sizeof(std::uint32_t)) {
        CHECK(std::bit_cast<std::uint32_t>(read) == std::bit_cast<std::uint32_t>(value));
    } else {
        CHECK(std::bit_cast<std::uint64_t>(read) == std::bit_cast<std::uint64_t>(value));
    }
}

// Version 1 called "current" "hp"; version 2 added "maximum" with the value 50 instead of the
// type's default. Version 3 is the current layout.
ith::Expected<void, Error> renameHp(Value& data) {
    if (Value* hp = data.find("hp")) {
        Value moved = std::move(*hp);
        data.remove("hp");
        data.set("current", std::move(moved));
    }
    return {};
}

ith::Expected<void, Error> addMaximum(Value& data) {
    if (data.kind() != Value::Kind::Object) {
        return ith::makeUnexpected(Error{.code = "test.not_an_object", .message = "data is not an object"});
    }
    if (data.find("maximum") == nullptr) {
        data.set("maximum", Value::integer(50));
    }
    return {};
}

constexpr serialization::Migration kHealthMigrations[] = {&renameHp, &addMaximum};
constexpr serialization::DocumentFormat kHealthFormat{
    .name = "test.health", .version = 3, .minimumVersion = 1, .migrations = kHealthMigrations};

std::string healthDocument(int version, const std::string& data) {
    return R"({"format": "test.health", "version": )" + std::to_string(version) + R"(, "data": )" + data + "}";
}

} // namespace

TEST_SUITE("serialization.text") {
    TEST_CASE("a document is format, version and data, with fields in declaration order") {
        ith::test::Health health;
        health.current = 12.5f;
        const auto text = serialization::writeText(kFormat, health);
        REQUIRE(text.hasValue());
        CHECK(*text == "{\n"
                       "  \"format\": \"test.text\",\n"
                       "  \"version\": 1,\n"
                       "  \"data\": {\n"
                       "    \"current\": 12.5,\n"
                       "    \"maximum\": 100\n"
                       "  }\n"
                       "}\n");
    }

    TEST_CASE("values are written as documented") {
        ith::test::Hit hit;
        hit.kind = ith::test::DamageKind::Poison;
        hit.abilities = static_cast<ith::test::Abilities>(3); // Jump | Swim
        hit.altitude = ith::test::Altitude::Deep;
        ith::test::Containers containers;
        containers.maybePoint = ith::test::Vector3{.x = 1.0f};
        ith::test::AllLeaves leaves;
        leaves.id = ith::Uuid(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
        leaves.u64 = std::numeric_limits<std::uint64_t>::max();
        leaves.i64 = std::numeric_limits<std::int64_t>::min();
        leaves.f64 = 0.1;

        const auto hitText = serialization::writeText(kFormat, hit);
        const auto containersText = serialization::writeText(kFormat, containers);
        const auto leavesText = serialization::writeText(kFormat, leaves);
        REQUIRE(hitText.hasValue());
        REQUIRE(containersText.hasValue());
        REQUIRE(leavesText.hasValue());
        CHECK(hitText->find("\"kind\": \"Poison\"") != std::string::npos);
        CHECK(hitText->find("\"abilities\": [\n      \"Jump\",\n      \"Swim\"\n    ]") != std::string::npos);
        CHECK(hitText->find("\"altitude\": \"Deep\"") != std::string::npos);
        CHECK(containersText->find("\"maybe\": null") != std::string::npos);
        CHECK(containersText->find("\"maybePoint\": {") != std::string::npos);
        CHECK(leavesText->find("\"id\": \"01234567-89ab-cdef-fedc-ba9876543210\"") != std::string::npos);
        CHECK(leavesText->find("\"u64\": 18446744073709551615") != std::string::npos);
        CHECK(leavesText->find("\"i64\": -9223372036854775808") != std::string::npos);
        CHECK(leavesText->find("\"f64\": 0.1") != std::string::npos);
    }

    TEST_CASE("floats read back bit-identical") {
        for (const float value :
             {0.0f, -0.0f, 1.0f, 0.1f, 1.0f / 3.0f, -1e-30f, 16777217.0f, std::numeric_limits<float>::denorm_min(),
              -std::numeric_limits<float>::denorm_min(), std::bit_cast<float>(0x007fffffu),
              std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
              std::numeric_limits<float>::lowest(), std::numeric_limits<float>::epsilon()}) {
            CAPTURE(value);
            checkTextRoundTrip(value);
        }
        for (const double value :
             {0.0, -0.0, 0.1, 1.0 / 3.0, 1e300, -1e-300, std::numeric_limits<double>::denorm_min(),
              std::bit_cast<double>(0x000fffffffffffffull), std::numeric_limits<double>::min(),
              std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(), 9007199254740993.0}) {
            CAPTURE(value);
            checkTextRoundTrip(value);
        }
        // Pseudo-random bit patterns over the whole range, normal and subnormal alike.
        std::uint64_t state = 0x9e3779b97f4a7c15ull;
        const auto next = [&state]() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };
        for (int index = 0; index < 2000; ++index) {
            const auto single = std::bit_cast<float>(static_cast<std::uint32_t>(next()));
            const auto twice = std::bit_cast<double>(next());
            if (std::isfinite(single)) {
                checkTextRoundTrip(single);
            }
            if (std::isfinite(twice)) {
                checkTextRoundTrip(twice);
            }
        }
    }

    TEST_CASE("infinities and NaN are not written") {
        for (const float value : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::quiet_NaN()}) {
            const auto text = serialization::writeText(kFormat, value);
            REQUIRE_FALSE(text.hasValue());
            CHECK(text.error().code == "serialization.non_finite");
        }
        ith::test::Vector3 point;
        point.y = std::numeric_limits<float>::quiet_NaN();
        const auto text = serialization::writeText(kFormat, point);
        REQUIRE_FALSE(text.hasValue());
        CHECK(text.error().path == "$.y");
    }

    TEST_CASE("wrong types and ranges fail with the path of the value") {
        const Error nested =
            readError<ith::test::gear::Loadout>(R"({"slots": ["Fire"], "history": [{}, {"where": {"x": "far"}}]})");
        CHECK(nested.code == "serialization.wrong_type");
        CHECK(nested.path == "$.history[1].where.x");

        struct Case {
            const char* data;
            const char* code;
            const char* path;
        };

        for (const Case& check : {
                 Case{R"({"i8": 128})", "serialization.out_of_range", "$.i8"},
                 Case{R"({"i8": -129})", "serialization.out_of_range", "$.i8"},
                 Case{R"({"u8": -1})", "serialization.wrong_type", "$.u8"},
                 Case{R"({"u64": 18446744073709551616})", "serialization.out_of_range", "$.u64"},
                 Case{R"({"i32": 1.5})", "serialization.wrong_type", "$.i32"},
                 Case{R"({"i32": 1e3})", "serialization.wrong_type", "$.i32"},
                 Case{R"({"f32": 1e39})", "serialization.out_of_range", "$.f32"},
                 Case{R"({"f32": "1"})", "serialization.wrong_type", "$.f32"},
                 Case{R"({"flag": 1})", "serialization.wrong_type", "$.flag"},
                 Case{R"({"text": 1})", "serialization.wrong_type", "$.text"},
                 Case{R"({"id": "not-a-uuid"})", "serialization.invalid_uuid", "$.id"},
                 Case{R"([])", "serialization.wrong_type", "$"},
             }) {
            CAPTURE(check.data);
            const Error error = readError<ith::test::AllLeaves>(check.data);
            CHECK(error.code == check.code);
            CHECK(error.path == check.path);
        }

        CHECK(readError<ith::test::Hit>(R"({"kind": "Lightning"})").code == "serialization.unknown_enumerator");
        CHECK(readError<ith::test::Hit>(R"({"kind": 1})").code == "serialization.wrong_type");
        const Error flag = readError<ith::test::Hit>(R"({"abilities": ["Jump", "Fly"]})");
        CHECK(flag.code == "serialization.unknown_enumerator");
        CHECK(flag.path == "$.abilities[1]");
        CHECK(readError<ith::test::Hit>(R"({"abilities": "Jump"})").code == "serialization.wrong_type");
        CHECK(readError<ith::test::Containers>(R"({"fixed": [1, 2]})").code == "serialization.wrong_size");
        CHECK(readError<ith::test::Containers>(R"({"names": "a"})").code == "serialization.wrong_type");
        CHECK(readError<ith::test::Containers>(R"({"maybe": "a"})").code == "serialization.wrong_type");
    }

    TEST_CASE("enum values without an enumerator are not written") {
        ith::test::Hit hit;
        hit.kind = static_cast<ith::test::DamageKind>(5);
        const auto kind = serialization::writeText(kFormat, hit);
        REQUIRE_FALSE(kind.hasValue());
        CHECK(kind.error().code == "serialization.invalid_enum");
        CHECK(kind.error().path == "$.kind");

        hit.kind = ith::test::DamageKind::Fire;
        hit.abilities = static_cast<ith::test::Abilities>(8);
        const auto abilities = serialization::writeText(kFormat, hit);
        REQUIRE_FALSE(abilities.hasValue());
        CHECK(abilities.error().code == "serialization.invalid_enum");
    }

    TEST_CASE("flag enums read as the OR of their names") {
        ith::test::Hit hit;
        REQUIRE(serialization::readText(document(R"({"abilities": ["Climb", "Jump", "None"]})"), kFormat, hit));
        CHECK(static_cast<std::uint32_t>(hit.abilities) == 5);
    }

    TEST_CASE("missing fields keep their defaults; optionals read null as empty") {
        ith::test::Containers containers;
        containers.maybe = 2.0;
        REQUIRE(serialization::readText(document(R"({"maybe": null, "names": ["a"]})"), kFormat, containers));
        CHECK_FALSE(containers.maybe.has_value());
        CHECK(containers.names == std::vector<std::string>{"a"});
        CHECK(containers.fixed == std::array<std::int32_t, 3>{});

        ith::test::Health health;
        REQUIRE(serialization::readText(document("{}"), kFormat, health));
        CHECK(health.current == 100.0f);
    }

    TEST_CASE("unknown fields are warnings, or errors when cooking") {
        const std::string text =
            R"({"format": "test.text", "version": 1, "extra": 1, "data": {"current": 5, "bogus": {"deep": 1}}})";
        ith::test::Health health;
        std::vector<Error> warnings;
        REQUIRE(serialization::readText(text, kFormat, health, {.source = "health.json", .warnings = &warnings}));
        CHECK(health.current == 5.0f);
        REQUIRE(warnings.size() == 2);
        CHECK(warnings[0].code == "serialization.unknown_field");
        CHECK(warnings[0].source == "health.json");
        CHECK(warnings[1].path == "$.bogus");

        const Error nested =
            readError<ith::test::Hit>(R"({"where": {"x": 1, "w": 2}})", serialization::UnknownFields::Fail);
        CHECK(nested.code == "serialization.unknown_field");
        CHECK(nested.path == "$.where.w");
        ith::test::Health untouched;
        const auto header =
            serialization::readText(text, kFormat, untouched, {.unknownFields = serialization::UnknownFields::Fail});
        REQUIRE_FALSE(header.hasValue());
        CHECK(header.error().code == "serialization.unknown_field");
    }

    TEST_CASE("a field reads under its FormerName, but not under both names") {
        ith::test::Health health;
        REQUIRE(serialization::readText(document(R"({"maxHealth": 7})"), kFormat, health));
        CHECK(health.maximum == 7.0f);

        const Error both = readError<ith::test::Health>(R"({"maximum": 1, "maxHealth": 2})");
        CHECK(both.code == "serialization.duplicate_field");
        CHECK(both.path == "$.maxHealth");
        CHECK(readError<ith::test::Health>(R"({"current": 1, "current": 2})").code == "serialization.duplicate_field");
    }

    TEST_CASE("Transient fields are neither written nor read") {
        ith::test::Inventory inventory;
        const auto text = serialization::writeText(kFormat, inventory);
        REQUIRE(text.hasValue());
        CHECK(text->find("m_capacity") != std::string::npos);
        CHECK(text->find("m_items") == std::string::npos);

        std::vector<Error> warnings;
        REQUIRE(serialization::readText(document(R"({"m_items": ["00000000-0000-0000-0000-000000000001"]})"), kFormat,
                                        inventory, {.warnings = &warnings}));
        CHECK(inventory.items().empty());
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].path == "$.m_items");
    }

    TEST_CASE("EditorOnly data is source data and stays in text") {
        ith::test::archive::Annotated annotated;
        annotated.label = "door";
        annotated.note.text = "check the hinge";
        const auto text = serialization::writeText(kFormat, annotated);
        REQUIRE(text.hasValue());
        ith::test::archive::Annotated read;
        REQUIRE(serialization::readText(*text, kFormat, read));
        CHECK(read.label == "door");
        CHECK(read.note.text == "check the hinge");
    }

    TEST_CASE("a failed read leaves the target unchanged") {
        ith::test::gear::Loadout target;
        target.slots = {ith::test::DamageKind::Fire};
        target.history.resize(1);
        target.history[0].amount = 3.0;
        const ith::test::gear::Loadout before = target;
        const std::string bad = R"({"slots": ["Poison", "Poison"], "history": [{"amount": 1}, {"amount": "x"}]})";
        const auto result = serialization::readText(document(bad), kFormat, target);
        REQUIRE_FALSE(result.hasValue());
        CHECK(ith::typeOf<ith::test::gear::Loadout>().equals(&target, &before));
    }

    TEST_CASE("document headers are checked") {
        ith::test::Health health;
        const auto check = [&health](const std::string& text, const char* code) {
            CAPTURE(text);
            const auto result = serialization::readText(text, kFormat, health, {.source = "h.json"});
            REQUIRE_FALSE(result.hasValue());
            CHECK(result.error().code == code);
            CHECK(result.error().source == "h.json");
        };
        check("[]", "serialization.not_a_document");
        check(R"({"format": "test.text", "version": 1})", "serialization.not_a_document");
        check(R"({"format": 1, "version": 1, "data": {}})", "serialization.not_a_document");
        check(R"({"format": "test.text", "version": "1", "data": {}})", "serialization.not_a_document");
        check(R"({"format": "test.text", "version": -1, "data": {}})", "serialization.not_a_document");
        check(R"({"format": "test.text", "version": 1, "data": {}, "data": {}})", "serialization.duplicate_field");
        check(R"({"format": "other", "version": 1, "data": {}})", "serialization.wrong_format");
        check(document("{}", "2"), "serialization.newer_version");
        check(document("{}", "0"), "serialization.version_too_old");
        check(document("{"), "serialization.syntax");
    }

    TEST_CASE("comments are allowed only when reading config") {
        const std::string text = "// window settings\n" + document(R"({"current": 3 /* hp */})");
        ith::test::Health health;
        CHECK_FALSE(serialization::readText(text, kFormat, health).hasValue());
        REQUIRE(serialization::readText(text, kFormat, health, {.syntax = serialization::JsonSyntax::Config}));
        CHECK(health.current == 3.0f);
    }

    TEST_CASE("older documents migrate step by step; failures leave the target unchanged") {
        ith::test::Health health;
        REQUIRE(serialization::readText(healthDocument(1, R"({"hp": 9})"), kHealthFormat, health));
        CHECK(health.current == 9.0f);
        CHECK(health.maximum == 50.0f);

        REQUIRE(serialization::readText(healthDocument(2, R"({"current": 8})"), kHealthFormat, health));
        CHECK(health.current == 8.0f);
        CHECK(health.maximum == 50.0f);

        REQUIRE(serialization::readText(healthDocument(3, R"({"current": 7})"), kHealthFormat, health));
        CHECK(health.maximum == 100.0f);

        const ith::test::Health before = health;
        const auto failed = serialization::readText(healthDocument(1, "[1]"), kHealthFormat, health);
        REQUIRE_FALSE(failed.hasValue());
        CHECK(failed.error().code == "serialization.migration_failed");
        CHECK(failed.error().message.find("version 2 to 3") != std::string::npos);
        CHECK(failed.error().message.find("test.not_an_object") != std::string::npos);
        CHECK(health.current == before.current);

        const auto written = serialization::writeText(kHealthFormat, health);
        REQUIRE(written.hasValue());
        CHECK(written->find("\"version\": 3") != std::string::npos);
        CHECK(serialization::readText(healthDocument(4, "{}"), kHealthFormat, health).error().code ==
              "serialization.newer_version");
    }

    TEST_CASE("nesting deeper than kMaxDepth is refused in both directions") {
        ith::test::archive::Node root;
        ith::test::archive::Node* leaf = &root;
        // The document object, then an object and an array per level.
        for (int level = 0; level < 40; ++level) {
            leaf = &leaf->children.emplace_back();
        }
        const auto written = serialization::writeText(kFormat, root);
        REQUIRE_FALSE(written.hasValue());
        CHECK(written.error().code == "serialization.too_deep");

        std::string deep;
        for (int level = 0; level < 40; ++level) {
            deep += R"({"children": [)";
        }
        deep += "{}";
        for (int level = 0; level < 40; ++level) {
            deep += "]}";
        }
        ith::test::archive::Node read;
        const auto result = serialization::readText(document(deep), kFormat, read);
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == "serialization.too_deep");
    }

    TEST_CASE("every prefix of a document fails cleanly") {
        ith::test::Containers containers;
        containers.names = {"a", "b"};
        containers.nested = {{1, 2}, {3}};
        containers.maybePoint = ith::test::Vector3{};
        const auto text = serialization::writeText(kFormat, containers);
        REQUIRE(text.hasValue());
        const std::size_t closing = text->rfind('}');
        for (std::size_t length = 0; length < closing; ++length) {
            ith::test::Containers read;
            CHECK_FALSE(serialization::readText(std::string_view(*text).substr(0, length), kFormat, read).hasValue());
        }
    }

    TEST_CASE("reads stay within the allocation budget and the size limit") {
        // 100000 empty objects take a few hundred KiB of text, but over 400 MiB as Bulky values.
        std::string many = "[{}";
        for (int index = 1; index < 100000; ++index) {
            many += ",{}";
        }
        many += "]";
        std::vector<ith::test::archive::Bulky> bulky;
        const auto objects = serialization::readText(document(many), kFormat, bulky);
        REQUIRE_FALSE(objects.hasValue());
        CHECK(objects.error().code == "serialization.too_large");
        CHECK(bulky.empty());

        // The JSON tree itself is checked before it is built.
        std::string nulls = "[null";
        for (int index = 1; index < 100000; ++index) {
            nulls += ",null";
        }
        nulls += "]";
        std::vector<std::optional<std::int32_t>> optionals;
        const auto tree =
            serialization::readText(document(nulls), kFormat, optionals, {.allocationBudget = std::size_t{1} << 20});
        REQUIRE_FALSE(tree.hasValue());
        CHECK(tree.error().code == "serialization.too_large");
        REQUIRE(serialization::readText(document(nulls), kFormat, optionals));
        CHECK(optionals.size() == 100000);

        ith::test::Health health;
        const auto config = serialization::readText(document(R"({"current": 1})"), kFormat, health, {.maxSize = 16});
        REQUIRE_FALSE(config.hasValue());
        CHECK(config.error().code == "serialization.too_large");
    }

    TEST_CASE("unknown-field warnings are capped") {
        std::string data = "{";
        for (int index = 0; index < 150; ++index) {
            data += (index == 0 ? "\"f" : ", \"f") + std::to_string(index) + "\": 1";
        }
        data += "}";
        ith::test::Health health;
        std::vector<Error> warnings;
        REQUIRE(serialization::readText(document(data), kFormat, health, {.warnings = &warnings}));
        REQUIRE(warnings.size() == 101);
        CHECK(warnings[99].path == "$.f99");
        CHECK(warnings[100].message == "50 more unknown fields are dropped");
        CHECK(serialization::readText(document(data), kFormat, health).hasValue());
    }

    TEST_CASE("containers hold exactly the elements of the document, not their defaults") {
        ith::test::archive::Defaults defaults;
        REQUIRE(defaults.nodes.size() == 1);
        REQUIRE(serialization::readText(document(R"({"nodes": [{}, {"value": 2}], "maybe": {}})"), kFormat, defaults));
        REQUIRE(defaults.nodes.size() == 2);
        CHECK(defaults.nodes[0].value == 0);
        CHECK(defaults.nodes[1].value == 2);
        REQUIRE(defaults.maybe.has_value());
        CHECK(defaults.maybe->value == 0);

        ith::test::archive::Defaults untouched;
        REQUIRE(serialization::readText(document("{}"), kFormat, untouched));
        CHECK(untouched.nodes[0].value == 5);
        CHECK(untouched.maybe->value == 6);
    }

    TEST_CASE("text from the document is shortened and escaped in messages") {
        const std::string name(200, 'x');
        const Error error = readError<ith::test::Hit>(R"({"kind": ")" + name + R"(\n"})");
        CHECK(error.code == "serialization.unknown_enumerator");
        CHECK(error.message.find("...") != std::string::npos);
        CHECK(error.message.find('\n') == std::string::npos);
        CHECK(error.message.size() < 150);

        const Error key = readError<ith::test::Health>(R"({"a\u0001b": 1})", serialization::UnknownFields::Fail);
        CHECK(key.path == R"($.a\x01b)");
    }
}
