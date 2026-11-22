// toValue and fromValue (ADR-0033): a reflected value as a Value tree without a document, with the
// text archive's rules.

#include "ith/foundation/Entity.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/TextArchive.h"

#include "ArchiveTypes.h"
#include "ReflectedTypes.h"

#include <doctest/doctest.h>

// doctest prints std::string operands of failed checks through an ostream.
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace {

namespace serialization = ith::serialization;
using ith::Entity;
using ith::Uuid;
using ith::test::Hit;
using ith::test::archive::Link;

constexpr serialization::DocumentFormat kFormat{.name = "test.value_tree", .version = 1};

Hit makeHit() {
    Hit hit;
    hit.kind = ith::test::DamageKind::Fire;
    hit.where = {.x = 1.5f, .y = -2.0f, .z = 0.25f};
    hit.target.current = 40.0f;
    hit.amount = 12.5;
    return hit;
}

Uuid toId(void*, Entity entity) {
    return Uuid(entity.index(), 7);
}

Entity toEntity(void*, const Uuid& id) {
    return Entity::fromParts(static_cast<std::uint32_t>(id.high()), 1, 1);
}

} // namespace

TEST_SUITE("serialization.value_tree") {
    TEST_CASE("toValue writes what writeText writes as the data, and fromValue reads it back") {
        const Hit hit = makeHit();
        const auto value = serialization::toValue(ith::typeOf<Hit>(), &hit);
        REQUIRE(value.hasValue());
        REQUIRE(value->kind() == serialization::Value::Kind::Object);
        CHECK(value->find("kind")->asString() == "Fire");

        const auto text = serialization::writeText(kFormat, hit);
        REQUIRE(text.hasValue());
        const auto data = serialization::readDocument(*text, kFormat);
        REQUIRE(data.hasValue());
        CHECK(*data == *value);

        Hit read;
        REQUIRE(serialization::fromValue(*value, ith::typeOf<Hit>(), &read).hasValue());
        CHECK(ith::typeOf<Hit>().equals(&read, &hit));
    }

    TEST_CASE("a leaf or container is a value of its own") {
        const float number = 2.5f;
        const auto leaf = serialization::toValue(ith::typeOf<float>(), &number);
        REQUIRE(leaf.hasValue());
        CHECK(leaf->numberText() == "2.5");

        const std::vector<std::string> names = {"a", "b"};
        const auto list = serialization::toValue(ith::typeOf<std::vector<std::string>>(), &names);
        REQUIRE(list.hasValue());
        std::vector<std::string> read;
        REQUIRE(serialization::fromValue(*list, ith::typeOf<std::vector<std::string>>(), &read).hasValue());
        CHECK(read == names);
    }

    TEST_CASE("errors carry paths from the value itself; a failed read leaves the target unchanged") {
        auto value = serialization::parseJson(R"({"where": {"x": "far"}, "amount": 3})");
        REQUIRE(value.hasValue());
        Hit target = makeHit();
        const auto result = serialization::fromValue(*value, ith::typeOf<Hit>(), &target);
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == "serialization.wrong_type");
        CHECK(result.error().path == "$.where.x");
        const Hit expected = makeHit();
        CHECK(ith::typeOf<Hit>().equals(&target, &expected));
    }

    TEST_CASE("unknown fields warn or fail") {
        auto value = serialization::parseJson(R"({"amount": 3, "extra": true})");
        REQUIRE(value.hasValue());
        Hit target;
        std::vector<serialization::Error> warnings;
        REQUIRE(serialization::fromValue(*value, ith::typeOf<Hit>(), &target, {.warnings = &warnings}).hasValue());
        CHECK(target.amount == 3.0);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].code == "serialization.unknown_field");
        CHECK(warnings[0].path == "$.extra");

        const auto strict = serialization::fromValue(*value, ith::typeOf<Hit>(), &target,
                                                     {.unknownFields = serialization::UnknownFields::Fail});
        REQUIRE_FALSE(strict.hasValue());
        CHECK(strict.error().code == "serialization.unknown_field");
    }

    TEST_CASE("entities need a mapping, both ways") {
        const Link link{.target = Entity::fromParts(5, 1, 1), .weight = 2};
        const auto refused = serialization::toValue(ith::typeOf<Link>(), &link);
        REQUIRE_FALSE(refused.hasValue());
        CHECK(refused.error().code == "serialization.no_entity_mapping");
        CHECK(refused.error().path == "$.target");

        const serialization::EntityMapping mapping{.toId = &toId, .toEntity = &toEntity};
        const auto value = serialization::toValue(ith::typeOf<Link>(), &link, &mapping);
        REQUIRE(value.hasValue());
        CHECK(value->find("target")->asString() == Uuid(5, 7).toString());

        Link read;
        const auto withoutMapping = serialization::fromValue(*value, ith::typeOf<Link>(), &read);
        REQUIRE_FALSE(withoutMapping.hasValue());
        CHECK(withoutMapping.error().code == "serialization.no_entity_mapping");
        REQUIRE(serialization::fromValue(*value, ith::typeOf<Link>(), &read, {.entities = &mapping}).hasValue());
        CHECK(read.target == link.target);
        CHECK(read.weight == 2);
    }

    TEST_CASE("the allocation budget applies") {
        auto value = serialization::parseJson(R"(["aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb"])");
        REQUIRE(value.hasValue());
        std::vector<std::string> read;
        const auto result =
            serialization::fromValue(*value, ith::typeOf<std::vector<std::string>>(), &read, {.allocationBudget = 16});
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == "serialization.too_large");
    }
}
