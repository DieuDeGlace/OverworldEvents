#include "../src/TownRules.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>

int main() {
    using namespace encounters;
    assert(town::referralMarker(false, false, false) == town::InProgress);
    assert(town::referralMarker(true, false, false) == town::HandIn);
    assert(town::referralMarker(true, true, false) == town::NoQuest);
    assert(town::referralMarker(false, false, true) == town::NoQuest);
    assert(town::eligible(3, 100.f, 1u << 15));
    for (int behaviour = 0; behaviour <= 8; ++behaviour)
        assert(town::eligible(behaviour, 100.f, 0) == (behaviour == 3));
    for (unsigned flag : {9u, 13u, 17u, 18u})
        assert(!town::eligible(3, 100.f, 1u << flag));
    assert(!town::eligible(3, 0.f, 0));
    assert(!town::eligible(3, std::numeric_limits<float>::quiet_NaN(), 0));
    assert(!town::eligible(3, std::numeric_limits<float>::infinity(), 0));
    bool seen[unsigned(Kind::Count)]{};
    for (uint64_t roll = 0; roll < 300; ++roll) {
        auto kind = town::kind(roll);
        assert(town::referralKind(kind));
        seen[unsigned(kind)] = true;
        auto count = enemyCount(kind, roll << 16);
        assert(kind == Kind::Rescue ? count == 1 : count >= 4 && count <= 6);
    }
    assert(seen[unsigned(Kind::Battle)] && seen[unsigned(Kind::Rescue)] &&
           seen[unsigned(Kind::Ambush)]);
    assert(!seen[unsigned(Kind::ChestSack)] && !seen[unsigned(Kind::ChestBoss)] &&
           !seen[unsigned(Kind::Bounty)]);
    assert(town::dryLand({8, 10, 9, 11, 8}));
    assert(!town::dryLand({-83, -84, -82, -83, -85})); // Island-village ocean target.
    assert(!town::dryLand({8, 9, -1, 10, 8}));         // One wet part of the footprint.
    assert(!town::dryLand({30, 50, 31, 32, 33}));      // Excessive slope.
    assert(!town::dryLand({8, 9, 10, 11, std::numeric_limits<float>::quiet_NaN()}));
    Site origin{-1000, 4000};
    auto outside = town::candidate(origin, 500, 0);
    assert(outside.x - origin.x == 628 && outside.y == origin.y);
    assert(!within(outside, origin, 500));
    auto farther = town::candidate(origin, 500, 8);
    assert(farther.x - outside.x == 128);
    assert(town::candidate(origin, -1, 0).x - origin.x == 256);
    assert(town::candidate(origin, 100000, 0).x - origin.x == 4224);
    puts("Town eligibility, objective types and bounded outside-town sites: PASS");
}
