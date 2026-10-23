#pragma once
#include <cstdint>
#include "QuestIcons.h"
#include <string>
#include <vector>

namespace cube {
class Game;
}

// Native world-map objective icons. The map routine collects every icon it
// draws into one list; a hook just before that list is sorted and drawn adds
// one icon per active event, using the native mission-tag sprite. Icons tilt,
// scale and sort with the map like native ones; nothing persistent is changed.
namespace encounters::mapmarkers {
struct Marker {
    int32_t x, y, z; // Blocks.
    questicons::Icon icon = questicons::Icon::Event;
};
// Installs the hook after checking the exact native instructions. On failure
// the caller keeps its fallback (the actor's own map visibility).
// `sprite`: missiontag (default), destination, star, cross, key or treasure.
bool initialize(const wchar_t *sprite, std::string *error);
bool active();
// Replaces the markers drawn from the next map frame on (none without a game).
void publish(cube::Game *game, const std::vector<Marker> &markers);
} // namespace encounters::mapmarkers
