#include "../src/EventRules.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace encounters;
    assert(floorDivide(-1, 64) == -1 && floorDivide(-64, 64) == -1);
    assert(floorDivide(-65, 64) == -2 && floorDivide(65, 64) == 1);
    unsigned seen[unsigned(Kind::Count)]{};
    for (int x = -12; x <= 12; ++x)
        for (int y = -12; y <= 12; ++y) {
            Region region{x, y};
            auto initial = plan(12345, region, 0, 0);
            auto reload = plan(12345, region, 0, 0);
            assert(initial.site.x == reload.site.x && initial.site.y == reload.site.y);
            for (unsigned cycle = 0; cycle < 20; ++cycle) {
                auto p = plan(12345, region, 0, cycle);
                assert(p.site.x >= int64_t(x) * RegionBlocks + 64);
                assert(p.site.x < int64_t(x + 1) * RegionBlocks - 64);
                assert(p.site.y >= int64_t(y) * RegionBlocks + 64);
                assert(p.site.y < int64_t(y + 1) * RegionBlocks - 64);
                ++seen[unsigned(p.kind)];
                assert(p.enemies <= 6);
                if (p.kind == Kind::Battle)
                    assert(p.enemies >= 4);
                if (p.kind == Kind::ChestSack)
                    assert(p.enemies == 0);
                unsigned mask = 0;
                for (unsigned enemy = 0; enemy < p.enemies; ++enemy) {
                    assert(!objectiveComplete(p.enemies, mask));
                    mask |= 1u << enemy;
                }
                assert(objectiveComplete(p.enemies, mask) == (p.enemies > 0));
                if (cycle) {
                    bool found = false;
                    for (unsigned attempt = 0; attempt < 16; ++attempt)
                        found |= relocating(plan(12345, region, 0, cycle, attempt).site,
                                            initial.site, cycle);
                    assert(found);
                }
            }
        }
    for (unsigned kind = 0; kind < NaturalKindCount; ++kind)
        assert(seen[kind] > 1000);
    assert(seen[unsigned(Kind::Ambush)] == 0);
    assert(enemyCount(Kind::Ambush, 0) == 4 && enemyCount(Kind::Ambush, 2ull << 16) == 6);
    assert(!objectiveComplete(0, 0xff) && !objectiveComplete(7, 0xff));
    auto deadline = cooldownDeadline(1000, 120);
    assert(deadline == 8200);
    assert(!canRestart(deadline - 1, deadline, 1000));
    assert(!canRestart(deadline, deadline, 0));
    assert(!canRestart(deadline, deadline, deadline - 59));
    assert(canRestart(deadline, deadline, deadline - 60));
    assert(!canRestart(100, 90, 101));
    assert(stars(2, false) == 2 && stars(2, true) == 3 && stars(100, true) == 4);
    assert(enemyCount(Kind::ChestSack, 100) == 0 && enemyCount(Kind::Bounty, 100) == 1);
    for (unsigned index = 0; index < 32; ++index) {
        auto site = testSite({-105694681, 64965890}, index);
        assert(within(site, {-105694681, 64965890}, 192));
        assert(!within(site, {-105694681, 64965890}, 48));
    }
    // Landmarks: verified native location types, lakes and villages excluded.
    Kind landmark = Kind::Count;
    for (int type : {18, 28, 29, 31})
        assert(landmarkKind(type, landmark) && landmark == Kind::Hill);
    for (int type : {34, 35})
        assert(landmarkKind(type, landmark) && landmark == Kind::Camp);
    for (int type : {9, 16, 37, 38, 39})
        assert(landmarkKind(type, landmark) && landmark == Kind::Shrine);
    for (int type : {0, 4, 5, 7, 8, 12, 13, 14, 15, 40, 47})
        assert(!landmarkKind(type, landmark));
    for (uint64_t roll = 0; roll < 64; ++roll) {
        auto hill = landmarkEnemies(Kind::Hill, roll << 16);
        auto camp = landmarkEnemies(Kind::Camp, roll << 16);
        auto shrine = landmarkEnemies(Kind::Shrine, roll << 16);
        assert(hill >= 4 && hill <= 6 && camp >= 5 && camp <= 6 && shrine >= 3 && shrine <= 4);
    }
    assert(landmarkEnemies(Kind::Battle, 0) == 0);
    assert(leader(Kind::Hill, 0) && !leader(Kind::Hill, 1) && leader(Kind::Bounty, 0));
    assert(!leader(Kind::Battle, 0) && !landmarkEvent(Kind::Ambush) && landmarkEvent(Kind::Camp));
    assert(FirstLandmarkSlot + LandmarkSlots <= 32);
    puts("Regional relocation, stable active plans, enemy caps, objectives and cooldown: PASS");
}
