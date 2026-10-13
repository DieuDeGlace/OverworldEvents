#include "../src/InstanceRules.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace encounters;
    StateStore state;
    auto &first = state.get(42, {0, 0}, 0);
    first.kind = Kind::Rescue;
    first.planned = true;
    first.enemyCount = 1;
    first.site = {8190, 400};
    EventKey same{42, {0, 0}, 0}, neighbor{42, {1, 0}, 1};
    assert(siteAvailable(state, same, first.site, 100));
    assert(!siteAvailable(state, neighbor, {8195, 400}, 100));
    assert(!siteAvailable(state, neighbor, {8190 + InstanceSpacing, 400}, 100));
    assert(siteAvailable(state, neighbor, {8191 + InstanceSpacing, 400}, 100));
    assert(siteAvailable(state, {43, {1, 0}, 1}, first.site, 100));
    // A pending plan reserves its footprint before native actors are created.
    auto &pending = state.get(42, {1, 0}, 2);
    pending = first;
    pending.region = {1, 0};
    pending.slot = 2;
    pending.site = {8700, 400};
    assert(!siteAvailable(state, neighbor, {8702, 400}, 100));
    first.rewardClaimed = true;
    first.deadlineUTCsecs = 1000;
    assert(!siteAvailable(state, neighbor, first.site, 999));
    assert(siteAvailable(state, neighbor, first.site, 1000));
    // Session restart replaces unfinished plans but preserves completed reward
    // records, their cooldowns, and instances belonging to a different world.
    pending.cycle = 5;
    pending.defeatedMask = 1;
    pending.objectiveDone = true;
    pending.enemyHPFraction[0] = .25f;
    auto &other = state.get(43, {1, 0}, 2);
    other = pending;
    other.world = 43;
    auto count = state.restartUnfinished(42);
    assert(count == 1);
    auto reset = state.find(42, {1, 0}, 2);
    assert(reset && !reset->planned && reset->cycle == 6);
    assert(reset->previousSite.x == 8700 && reset->previousSite.y == 400);
    assert(reset->defeatedMask == 0 && !reset->objectiveDone);
    assert(reset->enemyHPFraction[0] == 1.f && reset->enemyCount == 0);
    assert(state.find(42, {0, 0}, 0)->deadlineUTCsecs == 1000);
    assert(state.find(42, {0, 0}, 0)->rewardClaimed);
    assert(state.find(43, {1, 0}, 2)->planned);
    assert(state.restartUnfinished(42) == 0);
    puts("Instance separation, pending reservations, cooldowns and session rerolls: PASS");
}
