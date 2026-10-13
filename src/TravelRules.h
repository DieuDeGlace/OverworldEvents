#pragma once
#include "EventRules.h"
#include <climits>
#include <cstddef>
#include <string_view>

namespace encounters::travel {
inline constexpr size_t ClassifyStolen = 14, QuestStolen = 19, AppendStolen = 16;
inline constexpr int QuestCookieOffset = 7, QuestCookieEnd = 11;
inline constexpr uintptr_t POIGenerationReturn = 0x11ccf1;
inline constexpr uintptr_t OceanGenerationReturn = 0x3181aa;

inline uint64_t gridKey(int x, int y) {
    return uint64_t(uint32_t(x)) << 32 | uint32_t(y);
}
inline bool isTravelQuest(int quest) {
    return quest >= 6 && quest <= 9;
}
inline bool isTravelItem(unsigned category, int id) {
    return category == 24 && (id == 1 || id == 2 || id == 3 || id == 18);
}
inline bool isGenerationCaller(uintptr_t imageBase, uintptr_t caller) {
    if (caller < imageBase)
        return false;
    auto relative = caller - imageBase;
    return relative == POIGenerationReturn || relative == OceanGenerationReturn;
}
inline bool suppressGeneratedItem(unsigned category, int id, uintptr_t imageBase,
                                  uintptr_t caller) {
    return isGenerationCaller(imageBase, caller) && isTravelItem(category, id);
}
inline uint64_t worldIdentity(uint32_t seed, std::string_view name) {
    uint64_t value = seed;
    for (unsigned char ch : name)
        value = (value ^ ch) * 1099511628211ull;
    return value;
}
inline unsigned dungeonIndex(uint64_t world, uint32_t seed, int gridX, int gridY) {
    return unsigned(mix(gridKey(gridX, gridY) ^ mix(world) ^ seed) % 4);
}
// Preserve the target of an instruction-relative 32-bit load when copying it
// into a trampoline. The native query's cookie load is the only such operand
// in the three verified stolen prologues.
inline bool relocateRIP(uintptr_t sourceEnd, uintptr_t destinationEnd, int32_t original,
                        int32_t &relocated) {
    if (sourceEnd > uint64_t(INT64_MAX) || destinationEnd > uint64_t(INT64_MAX))
        return false;
    auto difference = int64_t(sourceEnd) - int64_t(destinationEnd);
    if (difference < int64_t(INT32_MIN) - original || difference > int64_t(INT32_MAX) - original)
        return false;
    relocated = int32_t(difference + original);
    return true;
}
} // namespace encounters::travel
