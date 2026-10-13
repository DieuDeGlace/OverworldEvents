#include "../src/EventObjectRules.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

namespace objects = encounters::objects;
struct Position {
    std::int64_t x, y, z;
};

int main() {
    assert(objects::ExamineBehaviour == 8);
    assert(objects::CaptiveAbility == 0x57);
    for (int race = 0; race <= 305; ++race) {
        bool cage = race >= 194 && race <= 197;
        bool chest = race >= 181 && race <= 184;
        assert(objects::cageRace(race) == cage);
        assert(objects::chestRace(race) == chest);
        assert(objects::objectRace(race) == (cage || chest || race == 242));
    }
    assert(objects::nativeKey(22, 6));
    assert(!objects::nativeKey(21, 6));
    assert(!objects::nativeKey(22, -1));
    assert(!objects::nativeKey(22, 20));

    const int heights[4] = {25, 25, 27, 28};
    const Position floor{100 * objects::FixedPerBlock, -50 * objects::FixedPerBlock,
                         7 * objects::FixedPerBlock};
    for (int race = 194; race <= 197; ++race) {
        assert(objects::cageModelHeight(race) == heights[race - 194]);
        auto cage = objects::cagePosition(floor, race, 2.5f, 2.5f, .5f);
        auto halfHeight = objects::cageMeshHalfHeight(race, 2.5f, 2.5f, .5f);
        // The visible bottom rests within one fixed unit of the support plane.
        double bottom = double(cage.z) / objects::FixedPerBlock - halfHeight;
        assert(std::abs(bottom - 7.) < 1. / objects::FixedPerBlock);
        assert(cage.x == floor.x && cage.y == floor.y);
        assert(cage.z > floor.z);
        for (float prisonerPhysics : {1.5f, 2.16f, 2.75f}) {
            auto captive = objects::captivePosition(cage, 2.75f, prisonerPhysics);
            assert(captive.x - cage.x == 1966);
            assert(captive.y - cage.y == 1966);
            // The prisoner and cage share the native physical floor, plus the
            // binder's clearance, for different captive body sizes.
            double captiveFloor = double(captive.z) / objects::FixedPerBlock - prisonerPhysics / 2.;
            double cageFloor = double(cage.z) / objects::FixedPerBlock - 2.75 / 2.;
            assert(std::abs((captiveFloor - cageFloor) - .03) < 1. / objects::FixedPerBlock);
        }
    }
    // Cage1's visible mesh center differs from a generic actor's physical
    // center; the model geometry must determine its support-plane placement.
    assert(objects::cageFloorOffset(194, 2.5f, 2.5f, .5f) == 91428);
    assert(objects::cageFloorOffset(194, 2.5f, 2.5f, .5f) > 90112);
    assert(objects::cageFloorOffset(194, 2.5f, 3.f, .5f) ==
           objects::cageFloorOffset(194, 3.f, 2.5f, .5f));
    assert(objects::cageFloorOffset(193, 2.5f, 2.5f, .5f) == 0);
    assert(objects::cageFloorOffset(194, 0.f, 0.f, .5f) == 0);
    assert(objects::cageFloorOffset(194, std::numeric_limits<float>::infinity(), 2.5f, .5f) == 0);
    puts("Native event object IDs, cage mesh anchor and captive geometry passed");
}
