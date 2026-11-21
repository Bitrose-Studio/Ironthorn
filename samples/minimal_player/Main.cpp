// minimal_player: the smallest player application (ADR-0017). It creates an empty world and
// exits, so CI can prove that a player links without editor or tools modules.

#include "ith/ecs/World.h"
#include "ith/foundation/Version.h"

#include <cstdio>

int main() {
    const ith::World world;
    std::printf("Ironthorn %.*s minimal player: world %u\n", static_cast<int>(ith::kEngineVersionString.size()),
                ith::kEngineVersionString.data(), static_cast<unsigned>(world.id()));
    return 0;
}
