#pragma once
#include "EventRules.h"
#include <string>

namespace cube {
class World;
}
namespace encounters::travel {
// Installed by the loader during initialization, before generation workers run.
// Converts only original native travel quests, retaining their POI coordinates.
bool initialize(std::string *error = nullptr);
void clearCache();
// True if the native planner put a utility-item quest (glider, boat, harp,
// whistle, ...) at the location returned by a location query at `query`.
// Reads the cached world plan; generates no terrain.
bool utilitySite(cube::World *world, Site query, Site center, bool ocean);
} // namespace encounters::travel
