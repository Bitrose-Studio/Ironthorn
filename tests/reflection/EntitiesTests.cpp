// The Entity leaf type, the containsEntities flag and the entity walker (ADR-0032).

#include "ith/reflection/Entities.h"

#include "ith/foundation/Entity.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include "ArchiveTypes.h"
#include "AssertCapture.h"
#include "ReflectedTypes.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through an ostream
#include <string>
#include <vector>

namespace {

using ith::Entity;
using ith::test::archive::Link;
using ith::test::archive::Node;
using ith::test::archive::NoEntities;
using ith::test::archive::Squad;

Entity entity(std::uint32_t index) {
    return Entity::fromParts(index, 1, 1);
}

// Moves every handle to world 2, keeping index and generation; counts the calls.
struct ToWorldTwo {
    int calls = 0;
};

Entity toWorldTwo(void* context, Entity source) {
    ++static_cast<ToWorldTwo*>(context)->calls;
    return source.isNull() ? Entity() : Entity::fromParts(source.index(), source.generation(), 2);
}

Entity moved(std::uint32_t index) {
    return Entity::fromParts(index, 1, 2);
}

} // namespace

TEST_SUITE("reflection.entities") {
    TEST_CASE("Entity is a leaf type that holds an entity") {
        const ith::TypeInfo& type = ith::typeOf<Entity>();
        CHECK(type.kind == ith::TypeKind::Entity);
        CHECK(type.name == "ith::Entity");
        CHECK(type.size == sizeof(Entity));
        CHECK(type.triviallyCopyable);
        CHECK(type.containsEntities);
        const Entity a = entity(3);
        const Entity b = entity(4);
        CHECK(type.equals(&a, &a));
        CHECK_FALSE(type.equals(&a, &b));
    }

    TEST_CASE("the kinds before Entity keep their values") {
        // The binary schema hash covers kind values, so Entity was appended.
        CHECK(static_cast<int>(ith::TypeKind::Optional) == 10);
        CHECK(static_cast<int>(ith::TypeKind::Entity) == 11);
    }

    TEST_CASE("containsEntities follows fields and elements") {
        CHECK(ith::typeOf<std::vector<Entity>>().containsEntities);
        CHECK(ith::typeOf<std::array<Entity, 3>>().containsEntities);
        CHECK(ith::typeOf<std::optional<Entity>>().containsEntities);
        CHECK(ith::typeOf<std::vector<std::optional<Entity>>>().containsEntities);
        CHECK(ith::typeOf<Link>().containsEntities);
        CHECK(ith::typeOf<Squad>().containsEntities);

        CHECK_FALSE(ith::typeOf<std::int32_t>().containsEntities);
        CHECK_FALSE(ith::typeOf<std::string>().containsEntities);
        CHECK_FALSE(ith::typeOf<ith::Uuid>().containsEntities);
        CHECK_FALSE(ith::typeOf<ith::Transform>().containsEntities);
        CHECK_FALSE(ith::typeOf<std::vector<float>>().containsEntities);
        CHECK_FALSE(ith::typeOf<ith::test::DamageKind>().containsEntities);
        CHECK_FALSE(ith::typeOf<NoEntities>().containsEntities);
        CHECK_FALSE(ith::typeOf<ith::test::Health>().containsEntities);
        // A field of another reflected struct counts, whatever that struct holds.
        CHECK(ith::typeOf<Node>().containsEntities);
    }

    TEST_CASE("remapEntities replaces every reflected entity at any depth") {
        Squad squad;
        squad.leader = entity(1);
        squad.members = {entity(2), Entity(), entity(3)};
        squad.pair = {entity(4), entity(5)};
        squad.target = entity(6);
        squad.link = Link{.target = entity(7), .weight = 9};
        squad.links = {Link{.target = entity(8)}, std::nullopt, Link{.target = entity(9)}};
        squad.cached = entity(10);
        squad.hidden = entity(11);

        ToWorldTwo state;
        ith::remapEntities(ith::typeOf<Squad>(), &squad, {.function = &toWorldTwo, .context = &state});

        CHECK(squad.leader == moved(1));
        CHECK(squad.members == std::vector<Entity>{moved(2), Entity(), moved(3)});
        CHECK(squad.pair == std::array<Entity, 2>{moved(4), moved(5)});
        CHECK(squad.target == moved(6));
        CHECK(squad.link.target == moved(7));
        CHECK(squad.link.weight == 9);
        REQUIRE(squad.links.size() == 3);
        CHECK(squad.links[0]->target == moved(8));
        CHECK_FALSE(squad.links[1].has_value());
        CHECK(squad.links[2]->target == moved(9));
        // Transient fields are remapped too; fields that aren't reflected are not.
        CHECK(squad.cached == moved(10));
        CHECK(squad.hidden == entity(11));
        // Null handles are passed to the map as well.
        CHECK(state.calls == 11);
    }

    TEST_CASE("remapEntities on an empty optional and empty containers calls nothing") {
        Squad squad;
        squad.target.reset();
        ToWorldTwo state;
        ith::remapEntities(ith::typeOf<Squad>(), &squad, {.function = &toWorldTwo, .context = &state});
        // leader, pair (2), link.target and cached: all null.
        CHECK(state.calls == 5);
    }

    TEST_CASE("remapEntities returns at once for a type without entities") {
        NoEntities value{.values = {1, 2}, .scale = 2.0f};
        ToWorldTwo state;
        ith::remapEntities(ith::typeOf<NoEntities>(), &value, {.function = &toWorldTwo, .context = &state});
        CHECK(state.calls == 0);
        CHECK(value.values == std::vector<std::int32_t>{1, 2});
    }

    TEST_CASE("remapEntities walks a recursive type") {
        Node tree{.value = 1, .children = {Node{.value = 2, .children = {Node{.value = 3}}}}};
        ToWorldTwo state;
        ith::remapEntities(ith::typeOf<Node>(), &tree, {.function = &toWorldTwo, .context = &state});
        CHECK(state.calls == 0);
        CHECK(tree.children.at(0).children.at(0).value == 3);
    }

    TEST_CASE("remapEntities on a bare Entity") {
        Entity value = entity(5);
        ToWorldTwo state;
        ith::remapEntities(ith::typeOf<Entity>(), &value, {.function = &toWorldTwo, .context = &state});
        CHECK(value == moved(5));
        CHECK(state.calls == 1);
    }

    TEST_CASE("remapEntities without a function asserts and changes nothing") {
        Link link{.target = entity(3)};
        const ith::test::AssertCapture capture;
        ith::remapEntities(ith::typeOf<Link>(), &link, {});
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 1);
#endif
        CHECK(link.target == entity(3));
    }
}
