#include "common.hpp"
#include "game_registry.hpp"
#include <simdjson.h>


auto main() -> int
{

    GameWorld game {};
    Entity player = game.create_from_archetype<PlayerArchetype>();


    game.destroy_entity(player);


    return 0;
}
