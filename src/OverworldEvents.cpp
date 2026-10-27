#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "EventRules.h"
#include "EventHealthRules.h"
#include "DifficultyRules.h"
#include "QuestIcons.h"
#include "EventObjectRules.h"
#include "InstanceRules.h"
#include "MapMarkers.h"
#include "TownRules.h"
#include "DialogueRules.h"
#include "NativeWorld.h"
#include "RuntimeAssets.h"
#include "StateStore.h"
#include "TravelLocations.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

// The SDK's Item constructor only zero-initializes these native value fields.
// Containers owned by this DLL use its CRT; game containers are mutated by the
// CubeMega bridge, which uses the game's allocator.
cube::Item::Item() {
    std::memset(this, 0, sizeof(*this));
}

namespace eventmod {
using namespace encounters;
constexpr unsigned Fish = 1u << 8, Named = 1u << 9, NoTalk = 1u << 13, CanPickup = 1u << 14, Friendly = 1u << 15,
                   Mini = 1u << 17, Boss = 1u << 18, Petrified = 1u << 22;
// Native EntityBehaviour values; examination objects use objects::ExamineBehaviour.
constexpr char HostileBehaviour = 1, VillagerBehaviour = 3, InanimateBehaviour = 7;
// Actor slots: 0-5 enemies, then the event NPC, its ally, the cage/sack and the chest.
constexpr unsigned Npc = 6, Companion = 7, Object = 8, Container = 9;
constexpr unsigned ActorCount = 10;
constexpr short MapMarker = 1 << 8;
struct Bridge {
    cube::Creature *(*spawn)(const LongVector3 *, const IntVector2 *, int, int, int) = nullptr;
    int (*drop)(cube::Game *, cube::Creature *, const cube::Item *, cube::Creature *) = nullptr;
    int (*gear)(cube::Creature *, const IntVector2 *, uint64_t, cube::Item *) = nullptr;
    void (*dialogue)(cube::Game *, cube::Creature *, const wchar_t *) = nullptr;
    int (*grantKey)(cube::Game *, cube::Creature *, const cube::Item *) = nullptr;
    int (*consumeKey)(cube::Game *, cube::Creature *, const cube::Item *) = nullptr;
};
struct Active {
    EventKey key;
    std::array<uint64_t, ActorCount> actors{};
    // Last known position (blocks) of the objective, for the world-map icon;
    // kept while its actor is unloaded so far-away events stay marked.
    mapmarkers::Marker marker{};
    bool markerKnown = false;
    // Spawn positions of the instance's objects (cage/sack, chest), held every
    // frame so they cannot be pushed around.
    std::array<LongVector3, ActorCount> anchors{};
    unsigned anchoredMask = 0;
};
struct SpawnRequest {
    EventKey key;
    std::array<LongVector3, ActorCount> points;
};
struct TownReferral {
    Site town;
    EventKey objective;
    unsigned cycle;
};
struct Data {
    std::recursive_mutex gate;
    StateStore state;
    std::vector<Active> active;
    std::vector<TownReferral> referrals;
    const Active *constructing = nullptr;
    // Every actor ID this mod spawned in the current world session. Only these
    // can be orphans; native creatures may use any ID, including the bridge's range.
    std::unordered_set<uint64_t> spawned;
    std::array<std::atomic<uint64_t>, 4> guardianIds{};
    struct EnemyDifficulty {
        std::atomic<uint64_t> id{0};
        std::atomic<unsigned> level{1};
    };
    std::array<EnemyDifficulty, 64> enemyDifficulty{};
    struct TownSite {
        Site site{};
        bool found = false;
        uint64_t until = 0;
    };
    // Villagers in one town share the procedural search, including failures.
    std::unordered_map<uint64_t, TownSite> townSites;
    Bridge bridge;
    Assets assets;
    cube::World *session = nullptr;
    uint64_t world = 0, nextTick = 0;
    bool enabled = true, dirty = false, loaded = false, assetLog = false;
    unsigned slots = 3, cap = 2, cooldown = 120;
    unsigned landmarkCap = 2; // Active landmark encounters; 0 disables them.
    Kind requestedTest = Kind::Count;
    unsigned testAttempt = 0;
    bool reportLandmarks = false; // /events landmarks: log nearby landmark locations.
    cube::Game *game = nullptr;   // The running game, for nameplate queries.
    // Villager nameplate hints (OverworldEventQuestMarker), refreshed once a second.
    struct Hint {
        int marker;
        uint64_t until;
    };
    std::unordered_map<uint64_t, Hint> hints;
};
static Data &data() {
    static Data value;
    return value;
}
static void log(const char *message) {
    FILE *file = nullptr;
    fopen_s(&file, "Mods/OverworldEvents.log", "a");
    if (file) {
        fprintf(file, "%s\n", message);
        fclose(file);
    }
}
static uint64_t utc() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    return ticks.QuadPart / 10000000ull - 11644473600ull;
}
static uint64_t worldIdentity(cube::World &world) {
    auto key = mix(uint32_t(world.seed));
    for (unsigned char character : world.world_name)
        key = mix(key ^ character);
    return key;
}
static bool connect(Bridge &bridge) {
    if (bridge.spawn)
        return true;
    auto module = GetModuleHandleW(L"CubeMegaMod-XPC-RU1.dll");
    if (!module)
        module = GetModuleHandleW(L"CubeMegaMod-XPC-RU1-AmbientWater.dll");
    auto version =
        module ? reinterpret_cast<unsigned (*)()>(GetProcAddress(module, "OverworldBridgeVersion"))
               : nullptr;
    // Version 2 validates gear for the actual recipient and reports delivery.
    if (!version || version() != 2)
        return false;
    Bridge candidate;
    candidate.spawn =
        reinterpret_cast<decltype(candidate.spawn)>(GetProcAddress(module, "SpawnOverworldEntity"));
    candidate.drop =
        reinterpret_cast<decltype(candidate.drop)>(GetProcAddress(module, "DropOverworldReward"));
    candidate.gear =
        reinterpret_cast<decltype(candidate.gear)>(GetProcAddress(module, "GenerateOverworldGear"));
    candidate.dialogue =
        reinterpret_cast<decltype(candidate.dialogue)>(GetProcAddress(module, "OverworldDialogue"));
    candidate.grantKey =
        reinterpret_cast<decltype(candidate.grantKey)>(GetProcAddress(module, "GrantOverworldKey"));
    candidate.consumeKey = reinterpret_cast<decltype(candidate.consumeKey)>(
        GetProcAddress(module, "ConsumeOverworldKey"));
    if (!candidate.spawn || !candidate.drop || !candidate.gear || !candidate.dialogue ||
        !candidate.grantKey || !candidate.consumeKey)
        return false;
    bridge = candidate;
    return true;
}
static bool flush(Data &value) {
    if (!value.dirty)
        return true;
    std::string error;
    if (value.state.save(L"Mods/OverworldEvents.state", &error)) {
        value.dirty = false;
        return true;
    }
    log(error.c_str());
    return false;
}
static EventRecord &record(Data &value, const EventKey &key) {
    return value.state.get(key.world, key.region, key.slot);
}
// Completed and failed instances stay in the world for the session; only
// unfinished ones count toward the active-event cap.
// Random and landmark encounters have separate caps.
static unsigned unfinished(const Data &value, bool landmarks) {
    unsigned count = 0;
    for (const auto &active : value.active) {
        const auto *saved = value.state.find(active.key.world, active.key.region, active.key.slot);
        count += saved && !saved->rewardClaimed && !saved->failed &&
                 landmarkEvent(saved->kind) == landmarks;
    }
    // An accepted, not-yet-loaded objective reserves capacity too.
    if (!landmarks)
        for (const auto &entry : value.state.entries()) {
            const auto &saved = entry.second;
            if (saved.world != value.world || !saved.townQuest || saved.rewardClaimed || saved.failed)
                continue;
            bool active = false;
            for (const auto &instance : value.active)
                active |= instance.key == entry.first;
            count += !active;
        }
    return count;
}
static bool owned(const Data &value, uint64_t id) {
    if (!id)
        return false;
    if (value.constructing)
        for (auto actor : value.constructing->actors)
            if (actor == id)
                return true;
    for (const auto &active : value.active)
        for (auto actor : active.actors)
            if (actor == id)
                return true;
    return false;
}
static bool guardian(const Data &value, uint64_t id) {
    if (!id)
        return false;
    for (const auto &entry : value.guardianIds)
        if (entry.load(std::memory_order_relaxed) == id)
            return true;
    return false;
}
static void registerGuardian(Data &value, uint64_t id) {
    if (guardian(value, id))
        return;
    for (auto &entry : value.guardianIds)
        if (!entry.load(std::memory_order_relaxed)) {
            entry.store(id, std::memory_order_relaxed);
            return;
        }
    log("Guardian registry is full; encounter cap should prevent this.");
}
static void unregisterGuardian(Data &value, uint64_t id) {
    for (auto &entry : value.guardianIds)
        if (entry.load(std::memory_order_relaxed) == id)
            entry.store(0, std::memory_order_relaxed);
}
static unsigned enemyLevel(const Data &value, uint64_t id) {
    for (const auto &entry : value.enemyDifficulty)
        if (entry.id.load(std::memory_order_acquire) == id)
            return entry.level.load(std::memory_order_relaxed);
    return 0;
}
static void trackEnemy(Data &value, uint64_t id, unsigned level) {
    for (auto &entry : value.enemyDifficulty)
        if (entry.id.load(std::memory_order_relaxed) == id ||
            !entry.id.load(std::memory_order_relaxed)) {
            entry.level.store(level, std::memory_order_relaxed);
            entry.id.store(id, std::memory_order_release);
            return;
        }
}
static void forgetEnemy(Data &value, uint64_t id) {
    for (auto &entry : value.enemyDifficulty)
        if (entry.id.load(std::memory_order_relaxed) == id)
            entry.id.store(0, std::memory_order_release);
}
static void attackPower(cube::Creature *creature, float *power) {
    if (creature && power)
        if (auto level = enemyLevel(data(), uint64_t(creature->id)))
            *power *= difficulty::damage(level);
}
static void maximumHP(cube::Creature *creature, float *hp) {
    if (!creature || !hp)
        return;
    auto &value = data();
    // HP callbacks can arrive while another mod holds its progression lock.
    // Read the small ID registry without entering the event/state mutex.
    *hp = health::normalizedMaximumHP(*hp, creature->entity_data.appearance.flags2,
                                      guardian(value, uint64_t(creature->id)));
    if (auto level = enemyLevel(value, uint64_t(creature->id)))
        *hp *= difficulty::health(level);
}
static cube::Creature *resident(cube::Game *game, uint64_t id) {
    if (auto actor = native::resident(game->host.world, id))
        return actor;
    return game->world ? native::resident(*game->world, id) : nullptr;
}
static float fraction(cube::Creature *creature) {
    auto maximum = native::maximumHP(creature);
    return std::isfinite(maximum) && maximum > 0
               ? std::clamp(creature->entity_data.HP / maximum, 0.f, 1.f)
               : 1.f;
}
// Remove an actor on every world copy. Inanimate, opened and at zero HP it can
// no longer be talked to, looted, targeted or shown on the map.
static void despawn(cube::Game *game, uint64_t id) {
    forgetEnemy(data(), id);
    for (auto world : {&game->host.world, game->world}) {
        if (!world)
            continue;
        if (auto actor = native::resident(*world, id)) {
            actor->entity_data.binary_toggles &= ~MapMarker;
            actor->entity_data.interaction_state = 2;
            actor->entity_data.hostility_type = InanimateBehaviour;
            actor->entity_data.HP = 0;
        }
    }
}
// Forget a role before removing it, so its removal is not seen as a death.
static void releaseRole(cube::Game *game, Active &active, unsigned index) {
    auto id = active.actors[index];
    forgetEnemy(data(), id);
    active.actors[index] = 0;
    despawn(game, id);
}
static void friendlyActor(cube::Creature *creature) {
    auto &flags = creature->entity_data.appearance.flags2;
    flags = (flags & ~(NoTalk | CanPickup | Petrified)) | Friendly;
}
static bool sameRegion(Region a, Region b) {
    return a.x == b.x && a.y == b.y;
}
static void nextCandidate(EventRecord &saved) {
    // Tentative positions may be discarded while exploring; only an activated
    // encounter is fixed/persisted. Keep the counter inside the state bounds.
    saved.attempt = (saved.attempt + 1) & 0xffffu;
}
static std::vector<cube::Creature *> enemies(cube::Game *game, Region region, const Data &value) {
    std::vector<cube::Creature *> pool;
    for (auto creature : game->host.world.creatures) {
        if (!creature || owned(value, uint64_t(creature->id)))
            continue;
        auto &entity = creature->entity_data;
        // Wraiths use a native home-position teleport route not initialized by
        // the common factory. Exclude that race rather than spawning it at 0,0.
        if (entity.hostility_type != HostileBehaviour || entity.HP <= 0 || entity.race == 276 ||
            aquaticRace(value.assets, entity.race) ||
            (entity.appearance.flags2 & (Named | Friendly | Mini | Boss | Fish)))
            continue;
        auto actual = native::region(game->world, native::position(creature));
        if (!sameRegion(actual, region))
            continue;
        // Sample only dry overworld natives. Aquatic and dungeon populations
        // do not become a land encounter's enemy pool.
        int height = 0;
        auto column = native::field(game->world, native::position(creature));
        int z = int(floorDivide(entity.position.z, 65536));
        if (!column || native::blockType(*column, z) == cube::Block::Water ||
            !native::ground(game->world, native::position(creature), height) ||
            !native::overworld(game->world, native::position(creature), region))
            continue;
        pool.push_back(creature);
        if (pool.size() >= 32)
            break;
    }
    return pool;
}
// Fill an encounter's enemy races/stars from the region's sampled natives.
static bool chooseEnemies(cube::Game *game, Region region, const Data &value, uint64_t roll,
                          EventRecord &saved, bool repairAquatic = false) {
    if (!saved.enemyCount)
        return true;
    auto pool = enemies(game, region, value);
    if (pool.empty())
        return false;
    for (unsigned index = 0; index < saved.enemyCount; ++index) {
        // Repair only invalid unfinished roles in an accepted legacy quest.
        // Keep its land enemies, kills, health fractions and destination.
        if (repairAquatic && ((saved.defeatedMask & (1u << index)) ||
                              !aquaticRace(value.assets, saved.enemyRaces[index])))
            continue;
        auto pick = pool[mix(roll + index) % pool.size()];
        using LevelQuery = int (*)(cube::Creature *);
        static auto query = [] {
            auto module = GetModuleHandleW(L"PyroProgression_v.1.4.dll");
            if (!module)
                module = GetModuleHandleW(L"PyroProgression.dll");
            return module ? reinterpret_cast<LevelQuery>(
                                GetProcAddress(module, "PyroProgressionCreatureLevel"))
                          : nullptr;
        }();
        auto playerLevel = game->world->local_creature->entity_data.level;
        int regionalLevel = query ? query(pick) : 1 + 8 * int(pick->entity_data.level);
        saved.difficultyLevel =
            std::max(saved.difficultyLevel,
                     unsigned(std::clamp(std::max(int(playerLevel), regionalLevel), 1, 10000000)));
        saved.enemyRaces[index] = pick->entity_data.race;
        saved.enemyStars[index] = stars(int(pick->entity_data.level), leader(saved.kind, index));
    }
    return true;
}
static EventRecord *unusedSlot(Data &value, Region region) {
    for (unsigned slot = 0; slot < value.slots; ++slot) {
        if (value.state.size() >= StateStore::MaxRecords &&
            !value.state.find(value.world, region, slot))
            continue;
        auto &saved = value.state.get(value.world, region, slot);
        if (!saved.planned && !saved.rewardClaimed && !saved.failed)
            return &saved;
    }
    return nullptr;
}
static bool available(const Assets &assets, Kind kind) {
    if (!chestEvent(kind) && kind != Kind::Ambush && assets.npcRaces.empty())
        return false;
    if (kind == Kind::Rescue && !assets.cageRace)
        return false;
    if (chestEvent(kind) && (!assets.keyCategory || !assets.keyId || *assets.keyCategory != 22))
        return false;
    return kind != Kind::ChestSack || bool(assets.sackRace);
}
static bool selectTownSite(Data &value, cube::Game *game, EventRecord &candidate,
                           const native::Location &village) {
    auto key = uint64_t(uint32_t(village.x)) << 32 | uint32_t(village.y);
    if (value.townSites.size() > 128)
        value.townSites.clear();
    auto &cached = value.townSites[key];
    auto now = GetTickCount64();
    auto available = [&](Site site) {
        return relocating(site, candidate.previousSite, candidate.cycle) &&
               siteAvailable(value.state, {candidate.world, candidate.region, candidate.slot},
                             site, utc());
    };
    if (now < cached.until) {
        if (!cached.found)
            return false;
        if (available(cached.site)) {
            candidate.site = cached.site;
            return true;
        }
    }
    cached = {{}, false, now + 5000};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto site = town::candidate({village.x, village.y}, village.radius, attempt);
        if (native::coordinate(site) && available(site) &&
            native::overworld(game->world, site, candidate.region) &&
            town::dryLand(native::predictedFootprint(game->world, site))) {
            candidate.site = cached.site = site;
            cached.found = true;
            return true;
        }
    }
    return false;
}
static bool footprint(cube::World *world, Site site, int &height, int radius, bool flat) {
    if (!native::ground(world, site, height, 8))
        return false;
    for (int x = -radius; x <= radius; ++x)
        for (int y = -radius; y <= radius; ++y) {
            int other = 0;
            if (!native::ground(world, {site.x + x, site.y + y}, other, 8) ||
                abs(other - height) > (flat ? 0 : 1))
                return false;
        }
    return true;
}
static const Site offsets[ActorCount] = {{-8, -7}, {8, -7}, {-10, 5}, {10, 5}, {-5, 12},
                                         {5, 12},  {0, 0},  {-3, 2},  {0, 0},  {10, -3}};
static bool needsPosition(const EventRecord &saved, unsigned index) {
    if (index < Npc)
        return index < saved.enemyCount && !(saved.defeatedMask & (1u << index));
    if (index == Npc)
        return !chestEvent(saved.kind) && saved.kind != Kind::Ambush && !landmarkEvent(saved.kind);
    if (index == Companion)
        return saved.kind == Kind::Battle;
    if (index == Object) // Chest point (chest and landmark events) or the cage.
        return chestEvent(saved.kind) || landmarkEvent(saved.kind) ||
               (saved.kind == Kind::Rescue && !saved.rewardClaimed);
    return saved.kind == Kind::ChestSack; // Separate sack offset.
}
// Actor slots an instance should currently have in the world.
static bool expectsRole(const EventRecord &saved, unsigned index) {
    if (index < Npc)
        return index < saved.enemyCount && !(saved.defeatedMask & (1u << index));
    if (index == Npc)
        return !chestEvent(saved.kind) && saved.kind != Kind::Ambush && !landmarkEvent(saved.kind);
    if (index == Companion)
        return saved.kind == Kind::Battle;
    if (index == Object) // Sack, or the cage of an unfreed captive.
        return saved.kind == Kind::ChestSack || captiveNeedsCage(saved.kind, saved.rewardClaimed);
    // Container: the chest; a landmark's reward chest appears once its group falls.
    return chestEvent(saved.kind) || (landmarkEvent(saved.kind) && saved.objectiveDone);
}
// The key sack lies well away from its chest so that finding it is part of the
// event: about 56 blocks out, in a world-seeded direction, on open dry ground.
static bool sackPoint(cube::Game *game, const EventRecord &saved, int center, Site &point,
                      int &height) {
    constexpr double Pi = 3.14159265358979323846;
    auto roll = plan(saved.world, saved.region, saved.slot, saved.cycle).roll;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        double angle = double((roll >> 8) % 360) * Pi / 180 + attempt * Pi / 4;
        point = {saved.site.x + int64_t(std::llround(56 * std::cos(angle))),
                 saved.site.y + int64_t(std::llround(56 * std::sin(angle)))};
        if (footprint(game->world, point, height, 1, true) && abs(height - center) <= 24 &&
            native::overworld(game->world, point, saved.region))
            return true;
    }
    return false;
}
static bool place(cube::Game *game, EventRecord &saved,
                  std::array<LongVector3, ActorCount> &points) {
    // Random encounters avoid native locations; a landmark encounter is placed
    // inside its location on purpose, so it only checks the region owner.
    bool landmark = landmarkEvent(saved.kind);
    auto allowed = [&](Site point) {
        return landmark ? sameRegion(native::region(game->world, point), saved.region)
                        : native::overworld(game->world, point, saved.region);
    };
    int center = 0;
    if (!allowed(saved.site) ||
        !footprint(game->world, saved.site, center, saved.kind == Kind::Rescue ? 3 : 1,
                   saved.kind == Kind::Rescue || chestEvent(saved.kind)))
        return false;
    for (unsigned actor = 0; actor < ActorCount; ++actor) {
        if (!needsPosition(saved, actor))
            continue;
        auto offset = offsets[actor];
        // The native hostile/NPC aggro radius is 20 blocks. A guardian starts
        // farther from the quest giver/prisoner, who stays at the event center.
        if (actor == 0 && guardianEvent(saved.kind))
            offset = {36, 0};
        if (actor == 0 && landmark)
            offset = {0, 0}; // The leader holds the landmark's center.
        Site point{saved.site.x + offset.x, saved.site.y + offset.y};
        int height = 0;
        if (actor == Container && saved.kind == Kind::ChestSack) {
            if (!sackPoint(game, saved, center, point, height))
                return false;
        } else if (!native::ground(game->world, point, height, 5) ||
                   abs(height - center) > (landmark ? 6 : 3) || !allowed(point))
            return false;
        points[actor] =
            LongVector3(point.x * 65536 + 32768, point.y * 65536 + 32768, int64_t(height) * 65536);
    }
    return true;
}
static cube::Creature *spawnActor(Data &value, Active &active, unsigned index,
                                  const LongVector3 &point, int race, int behaviour, int stars) {
    IntVector2 region(active.key.region.x, active.key.region.y);
    auto creature = value.bridge.spawn(&point, &region, race, behaviour, stars);
    if (creature) {
        native::setHome(creature, point);
        active.actors[index] = uint64_t(creature->id);
        value.spawned.insert(uint64_t(creature->id));
    }
    return creature;
}
static void nameBoss(cube::Creature *creature, uint64_t roll) {
    const char *first[] = {"Vor", "Mor", "Kel", "Zar", "Ash", "Grim", "Bel", "Fen"};
    const char *last[] = {"gath", "drak", "thorn", "mire", "gorn", "zul", "bane", "fang"};
    auto &entity = creature->entity_data;
    entity.appearance.flags2 = (entity.appearance.flags2 & ~(Friendly | Mini)) | Named | Boss;
    snprintf(entity.name, sizeof(entity.name), "%s%s", first[roll % 8], last[(roll >> 16) % 8]);
    entity.appearance.graphics_scale *= 1.3f;
    entity.appearance.hitbox_scale *= 1.3f;
    entity.appearance.physics_scale *= 1.3f;
}
static void noncombatObject(cube::Creature *creature) {
    auto &entity = creature->entity_data;
    entity.hostility_type = objects::ExamineBehaviour;
    entity.HP = 1.f; // Native objects need positive liveness, without health UI.
    std::memset(&entity.equipment, 0, sizeof(entity.equipment));
    entity.appearance.flags2 &= ~(NoTalk | CanPickup | Petrified | Friendly | Named | Mini | Boss);
    entity.velocity = FloatVector3(0, 0, 0);
    entity.acceleration = FloatVector3(0, 0, 0);
    entity.interaction_state = 1;
}
static void setPosition(cube::Creature *creature, const LongVector3 &position) {
    native::setStationaryPosition(creature, position);
}
static void placeContainer(cube::Creature *creature, const LongVector3 &floor) {
    auto &entity = creature->entity_data;
    const auto &appearance = entity.appearance;
    entity.pitch = entity.roll = 0.f;
    setPosition(creature,
                objects::containerPosition(floor, int(entity.race), appearance.graphics_scale,
                                           appearance.hitbox_scale, appearance.physics_scale,
                                           appearance.chest_scale, appearance.chest_position.z));
    native::setHome(creature, entity.position);
}
static void bindRescue(cube::Game *game, const Active &active) {
    auto cage = native::resident(game->host.world, active.actors[Object]);
    auto npc = native::resident(game->host.world, active.actors[Npc]);
    if (!cage || !npc || cage->entity_data.HP <= 0 || npc->entity_data.HP <= 0)
        return;
    const auto &appearance = cage->entity_data.appearance;
    auto position = objects::captivePosition(cage->entity_data.position, appearance.physics_scale,
                                             npc->entity_data.appearance.physics_scale);
    setPosition(npc, position);
    auto &entity = npc->entity_data;
    entity.hostility_type = objects::ExamineBehaviour;
    entity.current_ability = objects::CaptiveAbility;
    entity.yaw = cage->entity_data.yaw;
    entity.pitch = cage->entity_data.pitch;
    entity.roll = cage->entity_data.roll;
    entity.velocity = entity.acceleration = entity.retreat = FloatVector3(0, 0, 0);
    // Cage +0x1a8 is the native prisoner ID used by the ordinary cage binder.
    auto prisoner = uint64_t(npc->id);
    std::memcpy(reinterpret_cast<unsigned char *>(cage) + 0x1a8, &prisoner, sizeof(prisoner));
}
static void freeRescue(cube::Game *game, const Active &active) {
    for (auto world : {&game->host.world, game->world}) {
        if (!world)
            continue;
        if (auto npc = native::resident(*world, active.actors[Npc])) {
            npc->entity_data.hostility_type = VillagerBehaviour;
            npc->entity_data.current_ability = 0;
            npc->entity_data.appearance.flags2 |= Friendly;
            npc->entity_data.HP = native::maximumHP(npc);
            native::allowRoaming(npc);
        }
        if (auto cage = native::resident(*world, active.actors[Object])) {
            uint64_t noPrisoner = 0;
            std::memcpy(reinterpret_cast<unsigned char *>(cage) + 0x1a8, &noPrisoner,
                        sizeof(noPrisoner));
            cage->entity_data.binary_toggles &= ~MapMarker;
            cage->entity_data.interaction_state = 2;
            cage->entity_data.HP = 0;
        }
    }
}
// Marks the objective on the world map: a native mission-tag icon when the map
// hook is installed, otherwise the objective actor's own map visibility.
static void objectiveMarker(cube::Game *game, const EventRecord &saved, Active &active) {
    bool landmark = landmarkEvent(saved.kind);
    unsigned index = chestEvent(saved.kind) || (landmark && saved.objectiveDone) ? Container : Npc;
    if (saved.kind == Kind::Ambush || (landmark && !saved.objectiveDone)) {
        index = 0;
        for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy)
            if (!(saved.defeatedMask & (1u << enemy)) && resident(game, active.actors[enemy])) {
                index = enemy;
                break;
            }
    }
    // Host/client replicas share an ID. Clear old anchors before selecting one,
    // including when the first member of a town-requested enemy group dies.
    for (auto world : {&game->host.world, game->world}) {
        if (!world)
            continue;
        for (auto id : active.actors)
            if (auto actor = native::resident(*world, id))
                actor->entity_data.binary_toggles &= ~MapMarker;
        if (!saved.rewardClaimed && !saved.failed && !mapmarkers::active())
            if (auto actor = native::resident(*world, active.actors[index]))
                actor->entity_data.binary_toggles |= MapMarker;
    }
    if (auto actor = resident(game, active.actors[index])) {
        const auto &p = actor->entity_data.position;
        active.marker = {int32_t(floorDivide(p.x, 65536)), int32_t(floorDivide(p.y, 65536)),
                         int32_t(floorDivide(p.z, 65536))};
        active.markerKnown = true;
    }
}
static mapmarkers::Marker siteMarker(cube::Game *game, Site site, questicons::Icon icon) {
    auto height = native::predictedHeight(game->world, site);
    int z = std::isfinite(height) && std::abs(height) < 100000 ? int(std::floor(height)) : 0;
    return {int32_t(site.x), int32_t(site.y), z, icon};
}
// Hands unfinished destinations to the map hook, including accepted objectives
// whose actors have not loaded yet. Completion moves the grey pin back to town.
static void publishMarkers(const Data &value, cube::Game *game) {
    if (!mapmarkers::active())
        return;
    std::vector<mapmarkers::Marker> markers;
    for (const auto &active : value.active) {
        const auto *saved = value.state.find(active.key.world, active.key.region, active.key.slot);
        if (!active.markerKnown || !saved || saved->rewardClaimed || saved->failed)
            continue;
        auto marker = active.marker;
        marker.icon = saved->townQuest ? questicons::Icon::Destination : questicons::Icon::Event;
        if (saved->townQuest && saved->objectiveDone)
            marker = siteMarker(game, saved->town, questicons::Icon::Destination);
        markers.push_back(marker);
    }
    for (const auto &entry : value.state.entries()) {
        const auto &saved = entry.second;
        if (saved.world != value.world || !saved.townQuest || saved.rewardClaimed || saved.failed)
            continue;
        bool active = false;
        for (const auto &instance : value.active)
            active |= instance.key == entry.first;
        if (!active)
            markers.push_back(siteMarker(game, saved.objectiveDone ? saved.town : saved.site,
                                          questicons::Icon::Destination));
    }
    mapmarkers::publish(game, markers);
}
static bool spawn(Data &value, cube::Game *game, EventRecord &saved,
                  const std::array<LongVector3, ActorCount> &points, Active *existing = nullptr) {
    Active active{{saved.world, saved.region, saved.slot}};
    if (existing)
        active = *existing;
    const auto before = active.actors;
    struct ConstructionScope {
        Data &data;
        const Active *previous;
        ConstructionScope(Data &data, const Active &active)
            : data(data), previous(data.constructing) {
            data.constructing = &active;
        }
        ~ConstructionScope() {
            data.constructing = previous;
        }
    } construction(value, active);
    auto missing = [&](unsigned index) { return !resident(game, active.actors[index]); };
    auto roll = plan(saved.world, saved.region, saved.slot, saved.cycle).roll;
    int friendlyRace = value.assets.npcRaces.empty()
                           ? -1
                           : value.assets.npcRaces[roll % value.assets.npcRaces.size()];
    if (captiveNeedsCage(saved.kind, saved.rewardClaimed) && missing(Object)) {
        auto cage = spawnActor(value, active, Object, points[Object], *value.assets.cageRace,
                               objects::ExamineBehaviour, 0);
        if (cage) {
            noncombatObject(cage);
            cage->entity_data.yaw = cage->entity_data.pitch = cage->entity_data.roll = 0.f;
            const auto &appearance = cage->entity_data.appearance;
            setPosition(cage,
                        objects::cagePosition(points[Object], int(cage->entity_data.race),
                                              appearance.graphics_scale, appearance.hitbox_scale,
                                              appearance.chest_scale));
        }
    }
    // A captive is only created inside an existing cage; without one it waits
    // for the cage's next retry instead of standing free outside it.
    bool captiveReady = !captiveNeedsCage(saved.kind, saved.rewardClaimed) || !missing(Object);
    if (!chestEvent(saved.kind) && saved.kind != Kind::Ambush && !landmarkEvent(saved.kind) &&
        missing(Npc) && captiveReady) {
        // The native behaviour-3 talk branch reaches CubeMega's existing hook.
        // Behaviour 4 follows a separate path and cannot deliver event dialogue.
        auto npc = spawnActor(value, active, Npc, points[Npc], friendlyRace,
                              saved.kind == Kind::Battle ? VillagerBehaviour
                                                         : objects::ExamineBehaviour,
                              2);
        // A failed NPC is retried like any missing role. Returning early here
        // would leave actors created above untracked, and the slot would spawn twice.
        if (npc) {
            friendlyActor(npc);
            if (saved.kind == Kind::Battle)
                npc->entity_data.HP = native::maximumHP(npc) * std::max(.05f, saved.npcHPFraction);
            else
                noncombatObject(npc);
            snprintf(npc->entity_data.name, 16, "%s", "Adventurer");
            if (saved.kind == Kind::Battle)
                native::allowRoaming(npc);
        }
    }
    if (saved.kind == Kind::Battle && missing(Companion)) {
        auto ally = spawnActor(value, active, Companion, points[Companion], friendlyRace,
                               VillagerBehaviour, 1);
        if (ally) {
            friendlyActor(ally);
            native::allowRoaming(ally);
        }
    }
    for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy) {
        if (saved.defeatedMask & (1u << enemy))
            continue;
        if (!missing(enemy))
            continue;
        if (leader(saved.kind, enemy))
            unregisterGuardian(value, active.actors[enemy]);
        auto creature = spawnActor(value, active, enemy, points[enemy], saved.enemyRaces[enemy],
                                   HostileBehaviour, saved.enemyStars[enemy]);
        if (!creature)
            continue;
        creature->entity_data.appearance.flags2 &= ~(Friendly | Named | Mini | Boss);
        native::allowRoaming(creature);
        trackEnemy(value, uint64_t(creature->id), saved.difficultyLevel);
        if (leader(saved.kind, enemy)) {
            registerGuardian(value, uint64_t(creature->id));
            nameBoss(creature, roll);
        }
        creature->entity_data.HP =
            native::maximumHP(creature) * std::clamp(saved.enemyHPFraction[enemy], .01f, 1.f);
    }
    if (saved.kind == Kind::ChestSack && missing(Object)) {
        auto sack = spawnActor(value, active, Object, points[Container], *value.assets.sackRace,
                               objects::ExamineBehaviour, 0);
        if (sack) {
            // Native examination objects are excluded from combat targeting and
            // NPC health UI. Keep only positive liveness; this sack carries no gear.
            noncombatObject(sack);
            placeContainer(sack, points[Container]);
        }
    }
    bool rewardChest = chestEvent(saved.kind) || (landmarkEvent(saved.kind) && saved.objectiveDone);
    if (rewardChest && missing(Container)) {
        auto chest = spawnActor(value, active, Container, points[Object], value.assets.chestRace,
                                objects::ExamineBehaviour, 0);
        if (chest) {
            noncombatObject(chest);
            placeContainer(chest, points[Object]);
            chest->entity_data.interaction_state = saved.rewardClaimed ? 2 : 1;
        }
    }
    bool complete = landmarkEvent(saved.kind) ? !rewardChest || active.actors[Container] != 0
                    : saved.kind == Kind::Ambush ? active.actors[0] != 0
                    : chestEvent(saved.kind)     ? active.actors[Container] != 0
                                                 : active.actors[Npc] != 0;
    complete &= saved.kind != Kind::Battle || active.actors[Companion] != 0;
    complete &= saved.kind != Kind::ChestSack || active.actors[Object] != 0;
    complete &= saved.kind != Kind::Rescue || saved.rewardClaimed || active.actors[Object] != 0;
    for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy)
        complete &= (saved.defeatedMask & (1u << enemy)) || active.actors[enemy] != 0;
    // Objects created now (after the cage's floor placement) become anchors.
    for (unsigned index : {Object, Container})
        if (active.actors[index] && active.actors[index] != before[index])
            if (auto object = native::resident(game->host.world, active.actors[index])) {
                active.anchors[index] = object->entity_data.position;
                active.anchoredMask |= 1u << index;
            }
    if (captiveNeedsCage(saved.kind, saved.rewardClaimed))
        bindRescue(game, active);
    else if (saved.kind == Kind::Rescue)
        freeRescue(game, active); // A reloaded, already freed adventurer.
    objectiveMarker(game, saved, active);
    if (!complete) {
        log("Incomplete native encounter retained; only missing actors will be retried.");
    }
    if (existing)
        *existing = active;
    else
        value.active.push_back(active);
    return complete;
}
static cube::Item keyItem(const Data &value, const EventRecord &saved) {
    cube::Item key;
    key.category = char(*value.assets.keyCategory);
    key.id = *value.assets.keyId;
    key.region = IntVector2(saved.region.x, saved.region.y);
    key.modifier = uint32_t(plan(saved.world, saved.region, saved.slot, saved.cycle).roll) | 1u;
    return key;
}
static bool grantKey(Data &value, cube::Game *game, EventRecord &saved) {
    auto key = keyItem(value, saved);
    if (!value.bridge.grantKey(game, game->world->local_creature, &key))
        return false;
    saved.keySearchFound = true;
    value.dirty = true;
    return true;
}
static void death(cube::Game *game, cube::Creature *creature);
// Actors this mod spawned that no instance owns any more (a replaced role)
// are removed: they could duplicate a role or reopen as a chest.
static void removeOrphans(Data &value, cube::Game *game) {
    std::vector<uint64_t> orphans;
    for (auto creature : game->host.world.creatures) {
        if (!creature || creature->entity_data.HP <= 0)
            continue;
        auto id = uint64_t(creature->id);
        if (value.spawned.count(id) && !owned(value, id))
            orphans.push_back(id);
    }
    for (auto id : orphans)
        value.spawned.erase(id);
    for (auto id : orphans)
        despawn(game, id);
    if (!orphans.empty()) {
        char message[96];
        snprintf(message, sizeof(message), "Removed %zu untracked event actor(s).", orphans.size());
        log(message);
    }
}
static void update(Data &value, cube::Game *game, Site player) {
    removeOrphans(value, game);
    for (auto it = value.active.begin(); it != value.active.end(); ++it) {
        auto &saved = record(value, it->key);
        if (!saved.rewardClaimed && !saved.failed)
            for (unsigned index = 0; index <= Npc; ++index)
                if (auto creature = native::resident(game->host.world, it->actors[index])) {
                    if (creature->entity_data.HP <= 0)
                        death(game, creature);
                    else if (index < saved.enemyCount)
                        saved.enemyHPFraction[index] = fraction(creature);
                    else if (index == Npc && saved.kind == Kind::Battle)
                        saved.npcHPFraction = fraction(creature);
                }
        if (captiveNeedsCage(saved.kind, saved.rewardClaimed) && !saved.failed) {
            // If the cage unloaded or failed, the captive goes too; both are
            // recreated together, the captive inside the new cage.
            if (!resident(game, it->actors[Object]) && it->actors[Npc])
                releaseRole(game, *it, Npc);
            bindRescue(game, *it);
        }
        // A finished encounter stays for the session: an opened chest, the
        // freed captive and the NPCs remain. Chunks may unload actors, so any
        // expected role is recreated when the site is approached again.
        if (!saved.failed && within(player, saved.site, 192)) {
            bool missing = false;
            for (unsigned index = 0; index < ActorCount; ++index)
                missing |= expectsRole(saved, index) && !resident(game, it->actors[index]);
            if (missing) {
                std::array<LongVector3, ActorCount> points{};
                bool placed = false;
                {
                    native::ZoneLock locked(game->world);
                    placed = locked.held && place(game, saved, points);
                }
                if (placed)
                    spawn(value, game, saved, points, &*it);
            }
        }
        objectiveMarker(game, saved, *it);
    }
}
static void testEncounter(Data &value, cube::Game *game, Site player, Region region) {
    if (value.requestedTest == Kind::Count)
        return;
    auto say = [&](const wchar_t *text) {
        value.bridge.dialogue(game, game->world->local_creature, text);
    };
    if (unfinished(value, false) >= value.cap) {
        say(L"Event limit reached. Finish an active encounter before testing another.");
        value.requestedTest = Kind::Count;
        return;
    }
    if (!available(value.assets, value.requestedTest)) {
        say(L"The native assets needed for that event are unavailable.");
        value.requestedTest = Kind::Count;
        return;
    }
    auto selected = unusedSlot(value, region);
    if (!selected) {
        say(L"This region has no unused event slot. Try testing in another region.");
        value.requestedTest = Kind::Count;
        return;
    }
    // Two candidate checks per tick, using the same restrictions as natural
    // encounters. A test never replaces an active or completed encounter.
    for (unsigned search = 0; search < 2 && value.testAttempt < 32; ++search) {
        auto candidate = *selected;
        candidate.kind = value.requestedTest;
        candidate.site = testSite(player, value.testAttempt++);
        auto roll = plan(value.world, region, candidate.slot, candidate.cycle).roll;
        candidate.enemyCount = enemyCount(candidate.kind, roll);
        if (!relocating(candidate.site, candidate.previousSite, candidate.cycle) ||
            !siteAvailable(value.state, {candidate.world, candidate.region, candidate.slot},
                           candidate.site, utc()))
            continue;
        std::array<LongVector3, ActorCount> points{};
        {
            native::ZoneLock locked(game->world);
            if (!locked.held)
                return;
            if (!place(game, candidate, points) ||
                !chooseEnemies(game, region, value, roll, candidate))
                continue;
        }
        candidate.planned = true;
        *selected = candidate;
        value.dirty = true;
        bool started = spawn(value, game, *selected, points);
        wchar_t message[180];
        auto dx = double(candidate.site.x - player.x), dy = double(candidate.site.y - player.y);
        swprintf_s(message, L"%s %.0f blocks away. Look for the chest or NPC marker on your map.",
                   started ? L"Test encounter started" : L"Native spawn needs a retry",
                   std::hypot(dx, dy));
        char debugSite[180];
        snprintf(debugSite, sizeof(debugSite),
                 "Test kind=%u at engine blocks %lld,%lld; spawned=%d", unsigned(candidate.kind),
                 candidate.site.x, candidate.site.y, started);
        log(debugSite);
        say(message);
        value.requestedTest = Kind::Count;
        return;
    }
    if (value.testAttempt >= 32) {
        say(L"No eligible dry overworld site found nearby. Move away from POIs or find open terrain.");
        value.requestedTest = Kind::Count;
    }
}
// Every frame: event objects (cage, sack, chest) stay exactly where they were
// placed, like native village props, so walking or fighting into them cannot
// push them; an unfreed captive stays bound inside its cage. Objects are never
// resized: they keep the native appearance generated for their race.
static void holdObjects(Data &value, cube::Game *game) {
    for (auto &active : value.active) {
        for (unsigned index : {Object, Container}) {
            if (!(active.anchoredMask & (1u << index)))
                continue;
            const auto &anchor = active.anchors[index];
            for (auto world : {&game->host.world, game->world}) {
                auto object = world ? native::resident(*world, active.actors[index]) : nullptr;
                if (!object || object->entity_data.HP <= 0)
                    continue;
                setPosition(object, anchor);
                auto &entity = object->entity_data;
                entity.velocity = entity.acceleration = entity.retreat = FloatVector3(0, 0, 0);
            }
        }
        const auto *saved = value.state.find(active.key.world, active.key.region, active.key.slot);
        if (saved && !saved->failed && captiveNeedsCage(saved->kind, saved->rewardClaimed))
            bindRescue(game, active);
    }
}
// The landmark record for a native location: the slot already bound to it,
// or a free reserved slot of its region (then bound to it via previousSite).
static EventRecord *landmarkRecord(Data &value, Region area, Site center) {
    auto start = unsigned(mix(uint64_t(center.x) * 0x9e3779b1ull ^ uint64_t(center.y)) %
                          LandmarkSlots);
    int freeSlot = -1;
    for (unsigned i = 0; i < LandmarkSlots; ++i) {
        unsigned slot = FirstLandmarkSlot + (start + i) % LandmarkSlots;
        const auto *existing = value.state.find(value.world, area, slot);
        if (existing && (within(existing->previousSite, center, 16) ||
                         (existing->planned && within(existing->site, center, 16))))
            return &value.state.get(value.world, area, slot);
        bool unused = !existing || (!existing->planned && !existing->rewardClaimed &&
                                    !existing->failed && !existing->previousSite.x &&
                                    !existing->previousSite.y);
        if (unused && freeSlot < 0 &&
            (existing || value.state.size() < StateStore::MaxRecords))
            freeSlot = int(slot);
    }
    if (freeSlot < 0)
        return nullptr;
    auto &saved = value.state.get(value.world, area, unsigned(freeSlot));
    saved.previousSite = center;
    value.dirty = true;
    return &saved;
}
// Landmark encounters: a natural native location close to the player that holds
// no utility item gets an encounter at the location itself. Runs under the
// terrain lock.
static void planLandmarks(Data &value, cube::Game *game, Site player, uint64_t now,
                          std::vector<SpawnRequest> &pending) {
    unsigned running = unfinished(value, true);
    for (const auto &request : pending)
        running += landmarkEvent(record(value, request.key).kind);
    // A native location query returns the location of the query point's own
    // 1024-block cell, and only within about 280 blocks of its center. Any
    // quarter of that disc holds a point of a 128-block lattice, so 7x7
    // queries find every location within 160 blocks, even at cell corners.
    // The query reads the world plan only; it does not generate terrain.
    std::vector<std::pair<Kind, Site>> found;
    std::vector<std::pair<int, Site>> utility; // Skipped: they hold a utility item.
    auto seen = [](const auto &list, Site center) {
        for (const auto &entry : list)
            if (entry.second.x == center.x && entry.second.y == center.y)
                return true;
        return false;
    };
    for (int dx = -3; dx <= 3; ++dx)
        for (int dy = -3; dy <= 3; ++dy) {
            Site query{player.x + dx * 128, player.y + dy * 128};
            auto location = native::location(game->world, query);
            Kind kind;
            Site center{location.x, location.y};
            if (!landmarkKind(location.type, kind) || seen(found, center) ||
                seen(utility, center))
                continue;
            if (travel::utilitySite(game->world, query, center, location.ocean))
                utility.push_back({location.type, center});
            else
                found.push_back({kind, center});
        }
    if (value.reportLandmarks) {
        value.reportLandmarks = false;
        char message[160];
        snprintf(message, sizeof(message), "Landmarks near you: %zu.", found.size());
        log(message);
        for (const auto &[kind, center] : found) {
            auto dx = double(center.x - player.x), dy = double(center.y - player.y);
            snprintf(message, sizeof(message), "  kind=%u at engine blocks %lld,%lld (%.0f away)",
                     unsigned(kind), center.x, center.y, std::hypot(dx, dy));
            log(message);
        }
        for (const auto &[type, center] : utility) {
            auto dx = double(center.x - player.x), dy = double(center.y - player.y);
            snprintf(message, sizeof(message),
                     "  skipped: location type %d holds a utility item, at engine blocks "
                     "%lld,%lld (%.0f away)",
                     type, center.x, center.y, std::hypot(dx, dy));
            log(message);
        }
    }
    unsigned searches = 0;
    for (const auto &[kind, center] : found) {
        // Only locations close enough to have loaded terrain; at most two
        // placement searches per second.
        if (running >= value.landmarkCap || searches >= 2)
            return;
        if (!within(player, center, 160))
            continue;
        auto area = native::region(game->world, center);
        auto saved = landmarkRecord(value, area, center);
        if (!saved)
            continue;
        EventKey key{value.world, area, saved->slot};
        bool busy = false;
        for (const auto &active : value.active)
            busy |= active.key == key;
        for (const auto &request : pending)
            busy |= request.key == key;
        if (busy || (saved->planned && !saved->rewardClaimed && !saved->failed))
            continue;
        if (saved->rewardClaimed || saved->failed) {
            // A finished landmark returns after its cooldown, as a new cycle.
            if (now < saved->deadlineUTCsecs)
                continue;
            auto cycle = saved->cycle + 1;
            *saved = EventRecord{};
            saved->world = key.world;
            saved->region = area;
            saved->slot = key.slot;
            saved->cycle = cycle;
            saved->previousSite = center;
            value.dirty = true;
        }
        ++searches;
        auto roll = plan(value.world, area, saved->slot, saved->cycle).roll;
        auto candidate = *saved;
        candidate.kind = kind;
        candidate.enemyCount = landmarkEnemies(kind, roll);
        std::array<LongVector3, ActorCount> points{};
        // Large centerpieces (a mana tree's trunk, a menhir) occupy the exact
        // center, so the outer rings stand the group beside them instead.
        static const Site nudges[] = {{0, 0},   {6, 0},   {-6, 0},  {0, 6},   {0, -6},
                                      {8, 8},   {-8, 8},  {8, -8},  {16, 0},  {-16, 0},
                                      {0, 16},  {0, -16}, {24, 24}, {-24, 24}, {24, -24},
                                      {-24, -24}};
        bool placed = false;
        for (const auto &nudge : nudges) {
            candidate.site = {center.x + nudge.x, center.y + nudge.y};
            if (siteAvailable(value.state, key, candidate.site, now) &&
                place(game, candidate, points) &&
                chooseEnemies(game, area, value, roll, candidate)) {
                placed = true;
                break;
            }
        }
        if (!placed)
            continue;
        candidate.planned = true;
        *saved = candidate;
        value.dirty = true;
        pending.push_back({key, points});
        ++running;
    }
}
static void tick(cube::Game *game) {
    auto &value = data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    if (value.enabled && game && game->host.running && game->world &&
        !game->world->local_creature)
        return; // Loading or respawning: keep tracking live actors, never re-spawn them.
    if (!value.enabled || !game || !game->host.running || !game->world) {
        if (value.loaded)
            flush(value);
        value.session = nullptr;
        value.game = nullptr;
        value.hints.clear();
        value.townSites.clear();
        mapmarkers::publish(nullptr, {});
        value.active.clear();
        value.spawned.clear();
        value.referrals.clear();
        for (auto &entry : value.guardianIds)
            entry.store(0, std::memory_order_relaxed);
        for (auto &entry : value.enemyDifficulty)
            entry.id.store(0, std::memory_order_release);
        value.requestedTest = Kind::Count;
        return;
    }
    value.game = game;
    holdObjects(value, game);
    auto nowTick = GetTickCount64();
    if (nowTick < value.nextTick)
        return;
    value.nextTick = nowTick + 1000;
    auto identity = worldIdentity(game->host.world);
    bool newSession = value.session != &game->host.world || value.world != identity;
    if (newSession) {
        value.active.clear();
        value.spawned.clear();
        value.referrals.clear();
        for (auto &entry : value.guardianIds)
            entry.store(0, std::memory_order_relaxed);
        for (auto &entry : value.enemyDifficulty)
            entry.id.store(0, std::memory_order_release);
        value.session = &game->host.world;
        value.world = identity;
        value.assets = {};
        value.hints.clear();
        value.townSites.clear();
        value.assetLog = false;
        travel::clearCache();
    }
    if (!value.loaded) {
        std::string error;
        if (GetFileAttributesW(L"Mods/OverworldEvents.state") != INVALID_FILE_ATTRIBUTES &&
            !value.state.load(L"Mods/OverworldEvents.state", &error)) {
            log(error.c_str());
            log("Events stopped to preserve the unreadable state file.");
            value.enabled = false;
            return;
        }
        value.loaded = true;
    }
    if (newSession && value.state.restartUnfinished(value.world)) {
        value.dirty = true;
        log("New world session: unfinished encounters rerolled; completed cooldowns retained.");
    }
    if (newSession)
        for (const auto &entry : value.state.entries()) {
            const auto &saved = entry.second;
            if (saved.world == value.world && saved.townQuest)
                value.referrals.push_back({saved.town, entry.first, saved.cycle});
        }
    if (!connect(value.bridge))
        return;
    if (!value.assets.ready)
        value.assets = resolveAssets(game->speech);
    if (!value.assets.ready)
        return;
    if (!value.assetLog) {
        char message[160];
        snprintf(message, sizeof(message), "Native assets: cage=%d sack=%d key=%d:%d NPC races=%zu",
                 value.assets.cageRace.value_or(-1), value.assets.sackRace.value_or(-1),
                 value.assets.keyCategory.value_or(-1), value.assets.keyId.value_or(-1),
                 value.assets.npcRaces.size());
        log(message);
        value.assetLog = true;
    }
    auto player = native::position(game->world->local_creature);
    auto region = native::region(game->world, player);
    auto now = utc();
    update(value, game, player);
    unsigned searches = 0;
    bool testing = value.requestedTest != Kind::Count;
    testEncounter(value, game, player, region);
    std::vector<SpawnRequest> pending;
    {
        native::ZoneLock locked(game->world);
        if (!locked.held)
            return;
        for (const auto &referral : value.referrals) {
            auto &saved = record(value, referral.objective);
            bool exists = false;
            for (const auto &active : value.active)
                exists |= active.key == referral.objective;
            if (exists || saved.cycle != referral.cycle || saved.failed || saved.rewardClaimed)
                continue;
            if (saved.awaitingSite &&
                !town::dryLand(native::predictedFootprint(game->world, saved.site))) {
                auto village = native::location(game->world, saved.town);
                if (!selectTownSite(value, game, saved, village))
                    continue;
                saved.attempt = 0;
                value.dirty = true;
            }
            if (!within(player, saved.site, 192))
                continue;
            auto candidate = saved;
            auto target = saved.site;
            std::array<LongVector3, ActorCount> points{};
            // Four checks per quest/tick keep terrain work bounded on mobile.
            // Continue around the same small objective area on the next tick.
            for (unsigned search = 0; search < (saved.awaitingSite ? 4u : 1u); ++search) {
                unsigned attempt = saved.awaitingSite ? saved.attempt % 33 : 0;
                saved.attempt = (saved.attempt + 1) % 33;
                candidate.site = town::nearby(target, attempt);
                if (!siteAvailable(value.state, referral.objective, candidate.site, now) ||
                    !place(game, candidate, points))
                    continue;
                bool aquatic = false;
                for (unsigned index = 0; index < saved.enemyCount; ++index)
                    aquatic |= !(saved.defeatedMask & (1u << index)) &&
                               aquaticRace(value.assets, saved.enemyRaces[index]);
                if ((saved.awaitingSite || aquatic) &&
                    !chooseEnemies(game, saved.region, value,
                                   plan(saved.world, saved.region, saved.slot, saved.cycle).roll,
                                   candidate, aquatic && !saved.awaitingSite))
                    continue;
                if (aquatic)
                    log("Accepted land quest: replaced aquatic enemy templates with regional land enemies.");
                candidate.awaitingSite = false;
                saved = candidate;
                value.dirty = true;
                pending.push_back({referral.objective, points});
                break;
            }
        }
        for (int rx = -1; rx <= 1; ++rx)
            for (int ry = -1; ry <= 1; ++ry)
                for (unsigned slot = 0; slot < value.slots; ++slot) {
                    Region area{region.x + rx, region.y + ry};
                    if (value.state.size() >= StateStore::MaxRecords &&
                        !value.state.find(value.world, area, slot))
                        continue;
                    auto &saved = value.state.get(value.world, area, slot);
                    bool exists = false;
                    for (const auto &active : value.active)
                        exists |= active.key == EventKey{value.world, area, slot};
                    if (saved.rewardClaimed || saved.failed) {
                        if (within(player, saved.site, 256)) {
                            if (saved.emptySince) {
                                saved.emptySince = 0;
                                value.dirty = true;
                            }
                        } else if (!saved.emptySince) {
                            saved.emptySince = now;
                            value.dirty = true;
                        }
                        if (exists || !canRestart(now, saved.deadlineUTCsecs, saved.emptySince))
                            continue;
                        auto previous = saved.site;
                        auto cycle = saved.cycle + 1;
                        saved = EventRecord{};
                        saved.world = value.world;
                        saved.region = area;
                        saved.slot = slot;
                        saved.cycle = cycle;
                        saved.previousSite = previous;
                        value.dirty = true;
                    }
                    if (testing || unfinished(value, false) + pending.size() >= value.cap)
                        continue;
                    if (exists || saved.townQuest)
                        continue;
                    auto planned = plan(value.world, area, slot, saved.cycle, saved.attempt);
                    auto site = saved.planned ? saved.site : planned.site;
                    if (!native::coordinate(site) || !within(player, site, 192) ||
                        (!saved.planned && within(player, site, 48))) {
                        if (!saved.planned)
                            nextCandidate(saved);
                        continue;
                    }
                    if (searches >= 2)
                        continue;
                    if (!siteAvailable(value.state, {saved.world, saved.region, saved.slot}, site,
                                       now)) {
                        if (!saved.planned)
                            nextCandidate(saved);
                        continue;
                    }
                    if (!available(value.assets, saved.planned ? saved.kind : planned.kind))
                        continue;
                    ++searches;
                    if (!saved.planned) {
                        if (!relocating(site, saved.previousSite, saved.cycle)) {
                            nextCandidate(saved);
                            continue;
                        }
                        saved.site = site;
                        saved.kind = planned.kind;
                        saved.enemyCount = planned.enemies;
                    }
                    std::array<LongVector3, ActorCount> points{};
                    if (!place(game, saved, points)) {
                        if (!saved.planned)
                            nextCandidate(saved);
                        continue;
                    }
                    if (!saved.planned && !chooseEnemies(game, area, value, planned.roll, saved))
                        continue;
                    saved.planned = true;
                    value.dirty = true;
                    pending.push_back({{saved.world, saved.region, saved.slot}, points});
                }
        if (!testing)
            planLandmarks(value, game, player, now, pending);
    }
    // Construct native actors only after releasing terrain locks. Appearance,
    // dialogue and host registration can enter other engine subsystems.
    for (const auto &request : pending)
        spawn(value, game, record(value, request.key), request.points);
    publishMarkers(value, game);
    flush(value);
}
static void death(cube::Game *game, cube::Creature *creature) {
    if (!game || !creature)
        return;
    auto &value = data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    forgetEnemy(value, uint64_t(creature->id));
    for (auto &active : value.active) {
        auto &saved = record(value, active.key);
        if (saved.failed || saved.rewardClaimed)
            continue;
        if (active.actors[Npc] && active.actors[Npc] == uint64_t(creature->id) &&
            !saved.rewardClaimed) {
            saved.failed = true;
            saved.deadlineUTCsecs = cooldownDeadline(utc(), value.cooldown);
            saved.emptySince = 0;
            value.dirty = true;
        }
        if (saved.failed)
            continue;
        for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy) {
            if (active.actors[enemy] != uint64_t(creature->id) ||
                (saved.defeatedMask & (1u << enemy)))
                continue;
            saved.defeatedMask |= 1u << enemy;
            if (leader(saved.kind, enemy))
                unregisterGuardian(value, active.actors[enemy]);
            if (objectiveComplete(saved.enemyCount, saved.defeatedMask)) {
                if (saved.kind == Kind::ChestBoss)
                    grantKey(value, game, saved);
                else
                    saved.objectiveDone = true;
            }
            value.dirty = true;
        }
    }
}
// Exactly one reward per instance. The claim is saved before the item drops so
// a crash cannot repeat it, and undone if the bridge rejects the item.
static bool deliver(Data &value, cube::Game *game, EventRecord &saved, const Active &active) {
    if (saved.rewardClaimed)
        return false;
    auto recipient = game->world->local_creature;
    cube::Item reward;
    IntVector2 area(saved.region.x, saved.region.y);
    auto roll = plan(saved.world, saved.region, saved.slot, saved.cycle).roll;
    // The bridge generates for this recipient's class and rejects anything
    // outside its native class gear (e.g. silk for mages, cotton for rogues).
    if (!value.bridge.gear(recipient, &area, roll, &reward))
        return false;
    auto unclaim = [&] {
        saved.rewardClaimed = false;
        saved.deadlineUTCsecs = 0;
        value.dirty = true;
    };
    saved.rewardClaimed = true;
    saved.deadlineUTCsecs = cooldownDeadline(utc(), value.cooldown);
    saved.emptySince = 0;
    value.dirty = true;
    if (!flush(value)) {
        unclaim();
        return false;
    }
    // Resolve a surviving corpse at delivery; no native pointers survive frames.
    auto source = native::resident(game->host.world, active.actors[0]);
    if (!value.bridge.drop(game, recipient, &reward, source)) {
        unclaim();
        flush(value);
        log("Reward rejected at delivery; the claim was undone so it can be retried.");
        return false;
    }
    return true;
}
// One of five lines for this situation (see DialogueRules.h), at random but
// never the same line twice in a row from the same villager.
static const wchar_t *line(cube::Creature *speaker, dialogue::Topic topic, Kind kind) {
    static dialogue::Picker picker;
    static uint64_t counter = 0;
    auto random = mix(GetTickCount64() ^ (++counter << 32) ^ uint64_t(speaker->id));
    return dialogue::pool(topic, kind)[picker.pick(uint64_t(speaker->id), topic, kind, random)];
}
// Offers a town-referred event (offer=true) or reminds the player of one
// already taken, with the distance to its objective area.
static void describeReferral(Data &value, cube::Game *game, cube::Creature *speaker,
                             const EventRecord &saved, bool offer) {
    auto origin = native::position(speaker);
    auto distance = std::hypot(double(saved.site.x - origin.x), double(saved.site.y - origin.y));
    wchar_t message[320];
    swprintf_s(message,
               line(speaker, offer ? dialogue::Topic::Offer : dialogue::Topic::Reminder,
                    saved.kind),
               distance);
    value.bridge.dialogue(game, speaker, message);
}
static bool townOffer(Data &value, cube::Game *game, const native::Location &village,
                      EventRecord &candidate) {
    if (!value.bridge.spawn || unfinished(value, false) >= value.cap)
        return false;
    Site center{village.x, village.y};
    auto region = native::region(game->world, center);
    auto selected = unusedSlot(value, region);
    if (!selected)
        return false;
    candidate = *selected;
    auto roll = plan(value.world, region, candidate.slot, candidate.cycle).roll;
    candidate.kind = town::kind(mix(roll ^ mix(uint64_t(center.x)) ^ uint64_t(center.y)));
    // A missing cage must not prevent every villager in a town offering work.
    if (!available(value.assets, candidate.kind))
        candidate.kind = Kind::Ambush;
    candidate.enemyCount = enemyCount(candidate.kind, roll);
    if (!selectTownSite(value, game, candidate, village))
        return false;
    candidate.attempt = 0;
    candidate.planned = candidate.townQuest = candidate.awaitingSite = true;
    candidate.town = center;
    candidate.difficultyLevel = unsigned(std::clamp(
        int(game->world->local_creature->entity_data.level), 1, 10000000));
    return true;
}
static int townInteraction(Data &value, cube::Game *game, cube::Creature *creature) {
    if (!value.enabled || !game->host.running || value.session != &game->host.world ||
        !value.assets.ready || !value.bridge.spawn)
        return 0;
    const auto &entity = creature->entity_data;
    if (!town::eligible(entity.hostility_type, entity.HP, entity.appearance.flags2) ||
        std::find(value.assets.npcRaces.begin(), value.assets.npcRaces.end(), int(entity.race)) ==
            value.assets.npcRaces.end())
        return 0;
    auto origin = native::position(creature);
    auto village = native::location(game->world, origin);
    // Type 5 is Steam's dictionary-resolved Village. Its center identifies this
    // town across generic villagers and chunk reloads, without modifying NPC IDs.
    if (village.type != 5 || !std::isfinite(village.influence) || village.influence <= 0)
        return 0;
    Site center{village.x, village.y};
    auto region = native::region(game->world, origin);
    for (auto it = value.referrals.begin(); it != value.referrals.end(); ++it) {
        if (it->town.x != center.x || it->town.y != center.y)
            continue;
        auto &saved = record(value, it->objective);
        if (saved.cycle != it->cycle) {
            value.referrals.erase(it);
            break;
        }
        auto say = [&](const wchar_t *words) { value.bridge.dialogue(game, creature, words); };
        if (saved.rewardClaimed)
            say(line(creature, dialogue::Topic::Thanks, saved.kind));
        else if (saved.failed)
            say(line(creature, dialogue::Topic::Over, saved.kind));
        else if (!saved.objectiveDone)
            describeReferral(value, game, creature, saved, false);
        else {
            auto active =
                std::find_if(value.active.begin(), value.active.end(), [&](const Active &entry) {
                    return entry.key == it->objective;
                });
            Active unloaded{it->objective};
            auto &instance = active != value.active.end() ? *active : unloaded;
            if (deliver(value, game, saved, instance)) {
                if (saved.kind == Kind::Rescue)
                    freeRescue(game, instance);
                objectiveMarker(game, saved, instance);
                say(line(creature, dialogue::Topic::Reward, saved.kind));
            } else
                say(L"Your reward is not ready. Please speak to me again in a moment.");
        }
        return 1;
    }
    // A town may direct the player to an existing instance instead of spawning
    // a second copy of it. The shared completion flag prevents double rewards.
    for (const auto &active : value.active) {
        auto &saved = record(value, active.key);
        if (saved.failed || saved.rewardClaimed || saved.townQuest ||
            !sameRegion(saved.region, region) || !town::referralKind(saved.kind))
            continue;
        saved.townQuest = true;
        saved.town = center;
        value.dirty = true;
        value.referrals.push_back({center, active.key, saved.cycle});
        value.hints.clear();
        publishMarkers(value, game);
        flush(value);
        describeReferral(value, game, creature, saved, true);
        return 1;
    }
    EventRecord candidate;
    if (!townOffer(value, game, village, candidate))
        return 0;
    auto &selected = value.state.get(candidate.world, candidate.region, candidate.slot);
    selected = candidate;
    value.dirty = true;
    value.referrals.push_back(
        {center, {candidate.world, candidate.region, candidate.slot}, candidate.cycle});
    value.hints.clear();
    publishMarkers(value, game);
    describeReferral(value, game, creature, selected, true);
    flush(value);
    return 1;
}
static int interaction(cube::Game *game, cube::Creature *creature) {
    if (!game || !game->world || !game->world->local_creature || !creature)
        return 0;
    auto &value = data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    for (auto &active : value.active) {
        unsigned actor = ActorCount;
        for (unsigned index = Npc; index < ActorCount; ++index)
            if (active.actors[index] == uint64_t(creature->id))
                actor = index;
        if (actor == ActorCount)
            continue;
        auto &saved = record(value, active.key);
        auto speaker = creature;
        if (saved.kind == Kind::Rescue && actor == Object)
            if (auto npc = resident(game, active.actors[Npc]))
                speaker = npc;
        auto say = [&](const wchar_t *message) { value.bridge.dialogue(game, speaker, message); };
        if (saved.failed)
            return 1;
        if (saved.rewardClaimed) {
            say(chestEvent(saved.kind) || landmarkEvent(saved.kind)
                    ? L"The chest has been emptied."
                    : L"Thank you for your help. Safe travels!");
            return 1;
        }
        if (actor == Object && saved.kind == Kind::ChestSack) {
            if (!saved.objectiveDone && grantKey(value, game, saved))
                say(L"You have the key for this chest.");
            else
                say(L"The sack is empty.");
            flush(value);
            return 1;
        }
        if (actor == Container && landmarkEvent(saved.kind)) {
            // The reward chest only exists once the group has fallen.
            if (!saved.objectiveDone) {
                say(L"Defeat the guardian and its followers first.");
                return 1;
            }
            if (deliver(value, game, saved, active)) {
                for (auto world : {&game->host.world, game->world})
                    if (auto chest = world ? native::resident(*world, active.actors[Container])
                                           : nullptr) {
                        chest->entity_data.interaction_state = 2;
                        chest->entity_data.binary_toggles &= ~MapMarker;
                    }
                say(L"You found equipment inside the chest.");
            } else
                say(L"Please try opening the chest again in a moment.");
            return 1;
        }
        if (actor == Container && chestEvent(saved.kind)) {
            auto open = [&] {
                creature->entity_data.interaction_state = 2;
                creature->entity_data.binary_toggles &= ~MapMarker;
                if (auto host = native::resident(game->host.world, active.actors[Container])) {
                    host->entity_data.interaction_state = 2;
                    host->entity_data.binary_toggles &= ~MapMarker;
                }
            };
            if (saved.objectiveDone) {
                if (deliver(value, game, saved, active)) {
                    open();
                    say(L"You found equipment inside the chest.");
                } else
                    say(L"Please try opening the chest again in a moment.");
                return 1;
            }
            if (saved.kind == Kind::ChestBoss &&
                objectiveComplete(saved.enemyCount, saved.defeatedMask))
                grantKey(value, game, saved);
            auto key = keyItem(value, saved);
            if (!value.bridge.consumeKey(game, game->world->local_creature, &key)) {
                say(saved.kind == Kind::ChestSack
                        ? L"This chest is locked. Look for its key nearby."
                        : L"This chest is locked. The guardian holds its key.");
                return 1;
            }
            saved.keyOwned = true;
            saved.objectiveDone = true;
            value.dirty = true;
            if (deliver(value, game, saved, active)) {
                open();
                say(L"You unlocked the chest and found equipment inside.");
            } else
                say(L"The chest is unlocked. Please try opening it again in a moment.");
            return 1;
        }
        if (actor != Npc && !(actor == Object && saved.kind == Kind::Rescue)) {
            say(L"Help our group, then speak to the adventurer.");
            return 1;
        }
        if (!saved.objectiveDone) {
            switch (saved.kind) {
            case Kind::Battle:
                say(L"Please help us defeat these enemies. Speak to me afterward for a reward.");
                break;
            case Kind::Rescue:
                say(L"The guardian is keeping me trapped. Defeat it and come back to free me.");
                break;
            case Kind::Bounty:
                say(L"A dangerous enemy is nearby. Defeat it, then return to me for a reward.");
                break;
            default:
                say(L"Unlock the nearby chest, then speak to me for your reward.");
                break;
            }
            return 1;
        }
        if (deliver(value, game, saved, active)) {
            if (saved.kind == Kind::Rescue)
                freeRescue(game, active);
            say(L"Thank you. This equipment is your reward.");
        } else
            say(L"Your reward is not ready. Please speak to me again in a moment.");
        return 1;
    }
    return townInteraction(value, game, creature);
}
static void initialize() {
    auto &value = data();
    auto setting = [](const wchar_t *name, int fallback) {
        return int(GetPrivateProfileIntW(L"OverworldEvents", name, fallback,
                                         L".\\Mods\\OverworldEvents.ini"));
    };
    value.enabled = native::supported() && setting(L"Enabled", 1);
    value.slots = unsigned(std::clamp(setting(L"EventsPerRegion", 3), 1, 8));
    value.cap = unsigned(std::clamp(setting(L"ActiveEventCap", 2), 1, 3));
    value.cooldown = unsigned(std::clamp(setting(L"CooldownMinutes", 120), 30, 1440));
    value.landmarkCap = unsigned(std::clamp(setting(L"LandmarkEventCap", 2), 0, 4));
    if (value.enabled && setting(L"ReplaceTravelLocations", 1)) {
        std::string error;
        if (!travel::initialize(&error))
            log(error.c_str());
        else
            log("Original glider/boat/reins/boots sites now generate native dungeons.");
    }
    if (value.enabled && setting(L"MapMarkers", 1)) {
        std::string error;
        wchar_t sprite[32]{};
        GetPrivateProfileStringW(L"OverworldEvents", L"MapMarkerSprite", L"missiontag", sprite,
                                 32, L".\\Mods\\OverworldEvents.ini");
        if (!mapmarkers::initialize(sprite, &error))
            log(error.c_str());
        else
            log("World-map objective icons installed; the first map frame logs what it draws.");
    }
    log(value.enabled ? "OverworldEvents initialized; native assets resolved on world entry."
                      : "OverworldEvents disabled or unsupported Steam executable.");
}
static int chat(std::wstring *message) {
    if (!message || message->compare(0, 7, L"/events") != 0)
        return 0;
    auto &value = data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    const wchar_t *commands[] = {L"/events test battle", L"/events test rescue",
                                 L"/events test bounty", L"/events test chest-sack",
                                 L"/events test chest-boss"};
    for (unsigned index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index)
        if (*message == commands[index]) {
            value.requestedTest = Kind(index);
            value.testAttempt = 0;
            return 1;
        }
    if (*message == L"/events landmarks") {
        value.reportLandmarks = true;
        return 1;
    }
    if (*message == L"/events status") {
        char status[180];
        snprintf(status, sizeof(status),
                 "Status: enabled=%d bridge=%d assets=%d active=%zu records=%zu", value.enabled,
                 value.bridge.spawn != nullptr, value.assets.ready, value.active.size(),
                 value.state.size());
        log(status);
        return 1;
    }
    return 0;
}
// Village quest status: offer (1), hand-in (2), in progress (3), or none (0).
// Called while nameplates are drawn: never waits for the event lock.
static int questMarker(cube::Creature *creature) {
    auto &value = data();
    std::unique_lock<std::recursive_mutex> lock(value.gate, std::try_to_lock);
    if (!lock.owns_lock() || !creature || !value.enabled || !value.game ||
        !value.game->world || !value.assets.ready || value.session != &value.game->host.world)
        return 0;
    const auto &entity = creature->entity_data;
    if (!town::eligible(entity.hostility_type, entity.HP, entity.appearance.flags2) ||
        std::find(value.assets.npcRaces.begin(), value.assets.npcRaces.end(),
                  int(entity.race)) == value.assets.npcRaces.end())
        return 0;
    auto now = GetTickCount64();
    auto id = uint64_t(creature->id);
    auto cached = value.hints.find(id);
    if (cached != value.hints.end() && now < cached->second.until)
        return cached->second.marker;
    int marker = 0;
    auto origin = native::position(creature);
    auto village = native::location(value.game->world, origin);
    if (village.type == 5 && std::isfinite(village.influence) && village.influence > 0) {
        bool referred = false;
        for (const auto &referral : value.referrals) {
            if (referral.town.x != village.x || referral.town.y != village.y)
                continue;
            referred = true;
            const auto *saved = value.state.find(referral.objective.world,
                                                 referral.objective.region,
                                                 referral.objective.slot);
            if (saved && saved->cycle == referral.cycle)
                marker = town::referralMarker(saved->objectiveDone, saved->rewardClaimed,
                                              saved->failed);
        }
        if (!referred) {
            auto region = native::region(value.game->world, origin);
            for (const auto &active : value.active) {
                const auto &saved = record(value, active.key);
                if (!saved.townQuest && !saved.failed && !saved.rewardClaimed &&
                    sameRegion(saved.region, region) && town::referralKind(saved.kind))
                    marker = 1;
            }
            EventRecord offer;
            if (!marker && townOffer(value, value.game, village, offer))
                marker = 1;
        }
    }
    if (value.hints.size() > 512)
        value.hints.clear();
    value.hints[id] = {marker, now + 1000};
    return marker;
}
} // namespace eventmod

class OverworldEvents : public GenericMod {
  public:
    void Initialize() override {
        OnPresentPriority = VeryLowPriority; // Crisp UI after scene antialiasing.
        eventmod::initialize();
    }
    void OnGameTick(cube::Game *game) override {
        eventmod::tick(game);
    }
    int OnChat(std::wstring *message) override {
        return eventmod::chat(message);
    }
    void OnCreatureDeath(cube::Game *game, cube::Creature *creature, cube::Creature *) override {
        eventmod::death(game, creature);
    }
    void OnPresent(IDXGISwapChain *swapChain, UINT, UINT flags) override {
        if ((flags & DXGI_PRESENT_TEST) || !encounters::native::supported())
            return;
        auto game = *reinterpret_cast<cube::Game **>(encounters::native::base() + 0x551a80);
        bool playing = game && game->world && game->world->local_creature;
        encounters::questicons::present(swapChain, game && game->gui.map_open, playing);
    }
    void OnCreatureAttackPowerCalculated(cube::Creature *creature, float *power) override {
        eventmod::attackPower(creature, power);
    }
    void OnCreatureSpellPowerCalculated(cube::Creature *creature, float *power) override {
        eventmod::attackPower(creature, power);
    }
    void OnCreatureHPCalculated(cube::Creature *creature, float *hp) override {
        eventmod::maximumHP(creature, hp);
    }
};
static_assert(sizeof(GenericMod) == 0x428, "CubeModLoader API 7.3 layout");
EXPORT GenericMod *MakeMod() {
    return new OverworldEvents();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}
EXPORT int OverworldEventTalk(cube::Game *game, cube::Creature *creature) {
    return eventmod::interaction(game, creature);
}
EXPORT int OverworldEventObjectInteraction(cube::Game *game, cube::Creature *creature) {
    return eventmod::interaction(game, creature);
}
EXPORT void OverworldEventCreatureDeath(cube::Game *game, cube::Creature *creature,
                                        cube::Creature *) {
    eventmod::death(game, creature);
}
EXPORT int OverworldEventQuestMarker(cube::Creature *creature) {
    return eventmod::questMarker(creature);
}
// Coordinates are supplied by Pyro at the native nameplate path, after the
// game's visibility and line-of-sight tests. No guessed camera transforms.
EXPORT void OverworldEventNameplate(cube::Creature *creature, float x, float y) {
    int marker = eventmod::questMarker(creature);
    if (marker)
        encounters::questicons::nameplate(uint64_t(creature->id), x, y,
            marker == 1 ? encounters::questicons::Icon::Offer
            : marker == 2 ? encounters::questicons::Icon::Event
                          : encounters::questicons::Icon::Destination);
}
EXPORT int IsOverworldEventEntity(uint64_t id) {
    auto &value = eventmod::data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    return eventmod::owned(value, id) ? 1 : 0;
}
