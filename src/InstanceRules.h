#pragma once
#include "StateStore.h"

namespace encounters {
constexpr int InstanceSpacing = 192;
// CubeMega's bridge (version 2) assigns event actors IDs only from this range.
constexpr uint64_t FirstEntityId = 0x6000000000000000ull;
constexpr uint64_t EntityIdLimit = 0x7fffffffffffffffull;
inline bool eventEntityId(uint64_t id) {
    return id >= FirstEntityId && id < EntityIdLimit;
}
// An unfreed captive exists only together with its cage: never cageless.
inline bool captiveNeedsCage(Kind kind, bool rewardClaimed) {
    return kind == Kind::Rescue && !rewardClaimed;
}
inline bool reservesSite(const EventRecord &record, uint64_t now) {
    return record.planned &&
           ((!record.rewardClaimed && !record.failed) || record.deadlineUTCsecs > now);
}
inline bool siteAvailable(const StateStore &state, const EventKey &candidate, Site site,
                          uint64_t now) {
    for (const auto &entry : state.entries()) {
        if (entry.first == candidate || entry.first.world != candidate.world)
            continue;
        if (reservesSite(entry.second, now) && within(site, entry.second.site, InstanceSpacing))
            return false;
    }
    return true;
}
} // namespace encounters
