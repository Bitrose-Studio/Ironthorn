// Entity values in the archives (ADR-0032): refused without an entity mapping, written as the
// Uuid the mapping gives with one, and read back through it.

#include "ith/foundation/Entity.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/TextArchive.h"

#include "ArchiveTypes.h"

#include <doctest/doctest.h>

// doctest prints std::string operands of failed checks through an ostream.
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace {

namespace serialization = ith::serialization;
using ith::Entity;
using ith::Uuid;
using ith::test::archive::Link;
using ith::test::archive::NoEntities;
using ith::test::archive::Squad;

constexpr serialization::DocumentFormat kFormat{.name = "test.entities", .version = 1};

// Two entities and their ids, as a level's stable ids would be; counts the calls.
struct Table {
    Entity first = Entity::fromParts(1, 1, 1);
    Entity second = Entity::fromParts(2, 1, 1);
    Uuid firstId{0x1111, 0x1};
    Uuid secondId{0x2222, 0x2};
    int calls = 0;
};

Uuid toId(void* context, Entity entity) {
    auto& table = *static_cast<Table*>(context);
    ++table.calls;
    if (entity == table.first) {
        return table.firstId;
    }
    return entity == table.second ? table.secondId : Uuid();
}

Entity toEntity(void* context, const Uuid& id) {
    auto& table = *static_cast<Table*>(context);
    ++table.calls;
    if (id == table.firstId) {
        return table.first;
    }
    return id == table.secondId ? table.second : Entity();
}

Squad makeSquad(const Table& table) {
    Squad squad;
    squad.leader = table.first;
    squad.members = {table.second, Entity(), table.first};
    squad.pair = {table.second, table.first};
    squad.target = table.second;
    squad.link = Link{.target = table.first, .weight = 3};
    squad.links = {Link{.target = table.second}, std::nullopt};
    squad.cached = table.second;
    squad.hidden = table.first;
    return squad;
}

} // namespace

TEST_SUITE("serialization.entities") {
    TEST_CASE("an Entity is refused without a mapping, in text and binary, writing and reading") {
        const Link link{.target = Entity::fromParts(1, 1, 1)};

        const auto text = serialization::writeText(kFormat, link);
        REQUIRE_FALSE(text.hasValue());
        CHECK(text.error().code == "serialization.no_entity_mapping");
        CHECK(text.error().path == "$.target");

        const auto blob = serialization::writeBinary(kFormat, link);
        REQUIRE_FALSE(blob.hasValue());
        CHECK(blob.error().code == "serialization.no_entity_mapping");
        CHECK(blob.error().path == "$.target");

        // A null handle too: the rule is about the field, not its value.
        const Link empty;
        CHECK(serialization::writeText(kFormat, empty).error().code == "serialization.no_entity_mapping");

        // A mapping without the function the direction needs counts as none.
        Table table;
        const serialization::EntityMapping readOnly{.toEntity = &toEntity, .context = &table};
        CHECK(serialization::writeText(kFormat, link, nullptr, &readOnly).error().code ==
              "serialization.no_entity_mapping");
        CHECK(serialization::writeBinary(kFormat, link, &readOnly).error().code == "serialization.no_entity_mapping");

        const serialization::EntityMapping both{.toId = &toId, .toEntity = &toEntity, .context = &table};
        const auto saved = serialization::writeText(kFormat, link, nullptr, &both);
        const auto cooked = serialization::writeBinary(kFormat, link, &both);
        REQUIRE(saved.hasValue());
        REQUIRE(cooked.hasValue());

        Link target{.weight = 42};
        const auto readText = serialization::readText(*saved, kFormat, target);
        REQUIRE_FALSE(readText.hasValue());
        CHECK(readText.error().code == "serialization.no_entity_mapping");
        const serialization::EntityMapping writeOnly{.toId = &toId, .context = &table};
        CHECK(serialization::readText(*saved, kFormat, target, {.entities = &writeOnly}).error().code ==
              "serialization.no_entity_mapping");
        const auto readBinary = serialization::readBinary(*cooked, kFormat, target);
        REQUIRE_FALSE(readBinary.hasValue());
        CHECK(readBinary.error().code == "serialization.no_entity_mapping");
        // A failed read leaves the target unchanged.
        CHECK(target.weight == 42);
    }

    TEST_CASE("a type without entities needs no mapping") {
        const NoEntities value{.values = {1, 2}};
        CHECK(serialization::writeText(kFormat, value).hasValue());
        CHECK(serialization::writeBinary(kFormat, value).hasValue());
    }

    TEST_CASE("with a mapping, entities are written as their ids and read back") {
        Table table;
        const serialization::EntityMapping mapping{.toId = &toId, .toEntity = &toEntity, .context = &table};
        const Squad squad = makeSquad(table);

        const auto text = serialization::writeText(kFormat, squad, nullptr, &mapping);
        REQUIRE_MESSAGE(text.hasValue(), text.error().toString());
        CHECK(text->find(table.firstId.toString()) != std::string::npos);
        CHECK(text->find(table.secondId.toString()) != std::string::npos);
        // The null handle is the nil Uuid.
        CHECK(text->find(Uuid().toString()) != std::string::npos);
        // Transient and unreflected entities are not saved: leader, 2 members, pair (2), target,
        // link, links[0]; the null member is written without calling the mapping.
        CHECK(table.calls == 8);

        table.calls = 0;
        Squad fromText;
        const auto read = serialization::readText(*text, kFormat, fromText, {.entities = &mapping});
        REQUIRE_MESSAGE(read.hasValue(), read.error().toString());
        CHECK(table.calls == 8);
        CHECK(fromText.leader == squad.leader);
        CHECK(fromText.members == squad.members);
        CHECK(fromText.pair == squad.pair);
        CHECK(fromText.target == squad.target);
        CHECK(fromText.link.target == squad.link.target);
        CHECK(fromText.links.size() == 2);
        CHECK(fromText.links[0]->target == table.second);
        CHECK_FALSE(fromText.links[1].has_value());
        CHECK(fromText.cached.isNull());
        CHECK(fromText.hidden.isNull());

        const auto blob = serialization::writeBinary(kFormat, squad, &mapping);
        REQUIRE_MESSAGE(blob.hasValue(), blob.error().toString());
        Squad fromBinary;
        const auto readBlob = serialization::readBinary(*blob, kFormat, fromBinary, {.entities = &mapping});
        REQUIRE_MESSAGE(readBlob.hasValue(), readBlob.error().toString());
        Squad expected = squad;
        expected.cached = Entity(); // Transient: not saved
        CHECK(ith::typeOf<Squad>().equals(&fromBinary, &expected));
        CHECK(fromBinary.cached.isNull());
    }

    TEST_CASE("an id the mapping doesn't know reads as what the mapping says") {
        Table table;
        const serialization::EntityMapping mapping{.toId = &toId, .toEntity = &toEntity, .context = &table};
        const Link link{.target = table.first};
        const auto text = serialization::writeText(kFormat, link, nullptr, &mapping);
        REQUIRE(text.hasValue());
        table.firstId = Uuid(0x9999, 0x9); // the saved id is no longer known
        Link read{.target = table.second};
        REQUIRE(serialization::readText(*text, kFormat, read, {.entities = &mapping}).hasValue());
        CHECK(read.target.isNull());
    }

    TEST_CASE("an Entity that isn't a UUID string is refused") {
        Table table;
        const serialization::EntityMapping mapping{.toId = &toId, .toEntity = &toEntity, .context = &table};
        const std::string text = R"({"format": "test.entities", "version": 1, "data": {"target": 12}})";
        Link read;
        const auto result = serialization::readText(text, kFormat, read, {.entities = &mapping});
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == "serialization.wrong_type");
        CHECK(result.error().path == "$.target");
    }

    TEST_CASE("the schema hash encodes an Entity like a Uuid of its own kind") {
        CHECK(serialization::schemaHash(ith::typeOf<Entity>()) != serialization::schemaHash(ith::typeOf<Uuid>()));
        CHECK(serialization::schemaHash(ith::typeOf<Squad>()) == serialization::schemaHash(ith::typeOf<Squad>()));
    }
}
