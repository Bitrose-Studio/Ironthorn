// The input action components in a world copy and an archive (ADR-0032): InputPlayer::controlled
// is a reflected, Transient entity link, so a copy remaps it and saving skips it.

#include "ith/ecs/WorldCopy.h"

#include "ith/ecs/World.h"
#include "ith/input_actions/InputActionComponents.h"
#include "ith/serialization/TextArchive.h"

#include <doctest/doctest.h>

#include <ostream> // doctest prints std::string operands through it
#include <string>

TEST_SUITE("input_actions.world_copy") {
    TEST_CASE("a copy remaps the entity a player controls") {
        ith::World source;
        const ith::Entity pawn = source.spawn();
        const ith::Entity player = source.spawn(ith::InputPlayer{.index = 1, .controlled = pawn});

        ith::World target;
        REQUIRE(ith::copyWorld(source, target));
        const auto inCopy = [&](ith::Entity entity) {
            return ith::Entity::fromParts(entity.index(), entity.generation(), target.id());
        };
        const ith::InputPlayer* copied = target.get<const ith::InputPlayer>(inCopy(player));
        REQUIRE(copied != nullptr);
        CHECK(copied->index == 1);
        CHECK(copied->controlled == inCopy(pawn));
        CHECK(source.get<const ith::InputPlayer>(player)->controlled == pawn);
    }

    TEST_CASE("saving a player needs no entity mapping, because the link is Transient") {
        constexpr ith::serialization::DocumentFormat kFormat{.name = "test.input_player", .version = 1};
        const ith::InputPlayer player{.index = 2, .controlled = ith::Entity::fromParts(3, 1, 1)};
        const auto text = ith::serialization::writeText(kFormat, player);
        REQUIRE_MESSAGE(text.hasValue(), text.error().toString());
        CHECK(text->find("controlled") == std::string::npos);
        ith::InputPlayer read;
        REQUIRE(ith::serialization::readText(*text, kFormat, read).hasValue());
        CHECK(read.index == 2);
        CHECK(read.controlled.isNull());
    }
}
