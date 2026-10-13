#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace encounters::objects {
inline constexpr int ExamineBehaviour = 8;
inline constexpr unsigned char CaptiveAbility = 0x57;
inline constexpr int SackRace = 242;
inline constexpr int KeyCategory = 22;
inline constexpr int RustyKeyId = 6;
inline constexpr std::int64_t FixedPerBlock = 65536;
inline constexpr std::int64_t CaptiveHorizontalOffset = 1966;

inline constexpr bool cageRace(int race) {
    return race >= 194 && race <= 197;
}
inline constexpr bool chestRace(int race) {
    return race >= 181 && race <= 184;
}
inline constexpr bool objectRace(int race) {
    return cageRace(race) || chestRace(race) || race == SackRace;
}
inline constexpr bool nativeKey(int category, int id) {
    return category == KeyCategory && id >= 0 && id <= 19;
}

// Native cage-base .cub dimensions: 20x20x25, 22x22x25, 24x24x27,
// 24x24x28 for races 194..197. All four native appearances have zero
// chest_position and chest_scale 0.5, so their mesh origin is at its center.
inline constexpr int cageModelHeight(int race) {
    return race == 194 || race == 195 ? 25 : race == 196 ? 27 : race == 197 ? 28 : 0;
}
inline float cageMeshHalfHeight(int race, float graphicsScale, float hitboxScale,
                                float chestScale) {
    // Inspected Steam renderer RVA 0x25f90 divides the larger appearance scale
    // by 11.2; RVA 0x27522..0x2755c centers the base by half its model dimensions.
    // Keep the native float arithmetic and require the caller's idle, upright
    // cage pose (pitch/roll zero, animation scale one).
    float scale = std::max(graphicsScale, hitboxScale);
    if (!cageRace(race) || !std::isfinite(graphicsScale) || graphicsScale <= 0.f ||
        !std::isfinite(hitboxScale) || hitboxScale <= 0.f || !std::isfinite(chestScale) ||
        chestScale <= 0.f)
        return 0.f;
    return float(cageModelHeight(race)) * .5f * (scale / 11.2f) * chestScale;
}
inline std::int64_t cageFloorOffset(int race, float graphicsScale, float hitboxScale,
                                    float chestScale) {
    return static_cast<std::int64_t>(
        cageMeshHalfHeight(race, graphicsScale, hitboxScale, chestScale) * float(FixedPerBlock));
}
template <typename Position>
inline Position cagePosition(Position floor, int race, float graphicsScale, float hitboxScale,
                             float chestScale) {
    floor.z += cageFloorOffset(race, graphicsScale, hitboxScale, chestScale);
    return floor;
}

inline std::int64_t captiveVerticalOffset(float cagePhysicsScale, float captivePhysicsScale) {
    // Native cage binder RVA 0x2a15f1..0x2a16f2: use each actor's physical center
    // and leave 0.03 block clearance. CVTTSS2SI truncates toward zero.
    return static_cast<std::int64_t>((.03f - cagePhysicsScale * .5f + captivePhysicsScale * .5f) *
                                     float(FixedPerBlock));
}
template <typename Position>
inline Position captivePosition(Position cage, float cagePhysicsScale, float captivePhysicsScale) {
    cage.x += CaptiveHorizontalOffset;
    cage.y += CaptiveHorizontalOffset;
    cage.z += captiveVerticalOffset(cagePhysicsScale, captivePhysicsScale);
    return cage;
}
} // namespace encounters::objects
