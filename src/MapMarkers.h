#pragma once
#include <cstdint>
#include "QuestIcons.h"
#include <string>
#include <vector>

namespace cube {
class Game;
}

// Positions are projected by the native map camera. Full-map sprites use the
// supplied quest atlas; native icons remain the minimap/unsupported-GPU fallback.
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
