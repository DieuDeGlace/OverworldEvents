#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace encounters {
enum class Kind : unsigned { Battle, Rescue, Bounty, ChestSack, ChestBoss, Count };
struct Region {
    int x = 0, y = 0;
};
struct Site {
    int64_t x = 0, y = 0;
};
inline uint64_t mix(uint64_t n) {
    n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ull;
    n = (n ^ (n >> 27)) * 0x94d049bb133111ebull;
    return n ^ (n >> 31);
}
inline uint64_t identity(uint64_t world, Region region, unsigned slot) {
    return mix(world ^ mix(uint32_t(region.x)) ^ (mix(uint32_t(region.y)) << 1) ^
               (uint64_t(slot) + 1) * 0x9e3779b97f4a7c15ull);
}
inline int64_t floorDivide(int64_t value, int64_t divisor) {
    return value / divisor - (value < 0 && value % divisor != 0);
}
inline bool within(Site a, Site b, int radius) {
    long double dx = (long double)a.x - b.x, dy = (long double)a.y - b.y;
    return dx * dx + dy * dy <= (long double)radius * radius;
}
struct Plan {
    Site site;
    Kind kind;
    unsigned enemies;
    uint64_t roll;
};
// A cycle is persisted. Reloading an active event preserves its plan; completing
// it advances the cycle so future encounters move around the region.
inline Plan plan(uint64_t world, Region region, unsigned slot, unsigned cycle,
                 unsigned attempt = 0) {
    auto roll = mix(identity(world, region, slot) ^ mix(uint64_t(cycle) + 1));
    auto position = mix(roll ^ mix(uint64_t(attempt) + 0x4f564552ull));
    auto kind = static_cast<Kind>(roll % unsigned(Kind::Count));
    return {{int64_t(region.x) * 1024 + 64 + int64_t(position % 896),
             int64_t(region.y) * 1024 + 64 + int64_t((position >> 32) % 896)},
            kind,
            kind == Kind::Battle      ? 4u + unsigned((roll >> 16) % 3)
            : kind == Kind::ChestSack ? 0u
                                      : 1u,
            roll};
}
inline bool relocating(Site next, Site previous, unsigned cycle) {
    return cycle == 0 || !within(next, previous, 128);
}
inline unsigned allEnemies(unsigned count) {
    return count >= 1 && count <= 6 ? (1u << count) - 1u : 0;
}
inline bool objectiveComplete(unsigned count, unsigned defeatedMask) {
    auto all = allEnemies(count);
    return all && (defeatedMask & all) == all;
}
inline bool chestEvent(Kind kind) {
    return kind == Kind::ChestSack || kind == Kind::ChestBoss;
}
inline uint64_t cooldownDeadline(uint64_t now, unsigned minutes) {
    auto seconds = uint64_t(std::clamp(minutes, 30u, 1440u)) * 60;
    return now > std::numeric_limits<uint64_t>::max() - seconds
               ? std::numeric_limits<uint64_t>::max()
               : now + seconds;
}
inline bool canRestart(uint64_t now, uint64_t deadline, uint64_t emptySince) {
    return deadline && now >= deadline && emptySince && now >= emptySince && now - emptySince >= 60;
}
// Native stars remain independent of numeric PyroProgression levels.
inline int stars(int nativeStars, bool boss) {
    return std::clamp(nativeStars + (boss ? 1 : 0), 0, 4);
}
} // namespace encounters
