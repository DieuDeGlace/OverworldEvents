#pragma once
#include <string>

namespace encounters::travel {
// Installed by the loader during initialization, before generation workers run.
// Converts only original native travel quests, retaining their POI coordinates.
bool initialize(std::string *error = nullptr);
void clearCache();
} // namespace encounters::travel
