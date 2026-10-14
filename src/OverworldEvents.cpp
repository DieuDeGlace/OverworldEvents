#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "EventRules.h"
#include "EventHealthRules.h"
#include "EventObjectRules.h"
#include "InstanceRules.h"
#include "TownRules.h"
#include "NativeWorld.h"
#include "RuntimeAssets.h"
#include "StateStore.h"
#include "TravelLocations.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

// The SDK's Item constructor only zero-initializes these native value fields.
// Containers owned by this DLL use its CRT; game containers are mutated by the
// CubeMega bridge, which uses the game's allocator.
cube::Item::Item() {
    std::memset(this, 0, sizeof(*this));
}

namespace eventmod {
using namespace encounters;
constexpr unsigned Named = 1u << 9, NoTalk = 1u << 13, CanPickup = 1u << 14, Friendly = 1u << 15,
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
    bool retiring = false;
    uint64_t finishAt = 0;
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
    std::array<std::atomic<uint64_t>, 4> guardianIds{};
    Bridge bridge;
    Assets assets;
    cube::World *session = nullptr;
    uint64_t world = 0, nextTick = 0;
    bool enabled = true, dirty = false, loaded = false, assetLog = false;
    unsigned slots = 3, cap = 2, cooldown = 120;
    Kind requestedTest = Kind::Count;
    unsigned testAttempt = 0;
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
static void maximumHP(cube::Creature *creature, float *hp) {
    if (!creature || !hp)
        return;
    auto &value = data();
    // HP callbacks can arrive while another mod holds its progression lock.
    // Read the small ID registry without entering the event/state mutex.
    *hp = health::normalizedMaximumHP(*hp, creature->entity_data.appearance.flags2,
                                      guardian(value, uint64_t(creature->id)));
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
            (entity.appearance.flags2 & (Named | Friendly | Mini | Boss)))
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
                          EventRecord &saved) {
    if (!saved.enemyCount)
        return true;
    auto pool = enemies(game, region, value);
    if (pool.empty())
        return false;
    for (unsigned index = 0; index < saved.enemyCount; ++index) {
        auto pick = pool[mix(roll + index) % pool.size()];
        saved.enemyRaces[index] = pick->entity_data.race;
        saved.enemyStars[index] = stars(int(pick->entity_data.level), guardianEvent(saved.kind));
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
        return !chestEvent(saved.kind) && saved.kind != Kind::Ambush;
    if (index == Companion)
        return saved.kind == Kind::Battle;
    if (index == Object)
        return chestEvent(saved.kind) || (saved.kind == Kind::Rescue && !saved.rewardClaimed);
    return saved.kind == Kind::ChestSack; // Separate sack offset.
}
static bool place(cube::Game *game, EventRecord &saved,
                  std::array<LongVector3, ActorCount> &points) {
    int center = 0;
    if (!native::overworld(game->world, saved.site, saved.region) ||
        !footprint(game->world, saved.site, center, saved.kind == Kind::Rescue ? 3 : 1,
                   saved.kind == Kind::Rescue))
        return false;
    for (unsigned actor = 0; actor < ActorCount; ++actor) {
        if (!needsPosition(saved, actor))
            continue;
        auto offset = offsets[actor];
        // The native hostile/NPC aggro radius is 20 blocks. A guardian starts
        // farther from the quest giver/prisoner, who stays at the event center.
        if (actor == 0 && guardianEvent(saved.kind))
            offset = {36, 0};
        Site point{saved.site.x + offset.x, saved.site.y + offset.y};
        int height = 0;
        if (!native::ground(game->world, point, height, 5) || abs(height - center) > 3 ||
            !native::overworld(game->world, point, saved.region))
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
    if (creature)
        active.actors[index] = uint64_t(creature->id);
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
    creature->entity_data.position = position;
    // The inspected native cage binder also updates the render interpolation
    // position. These fields fall inside the SDK's still-unnamed gapAC8.
    static_assert(offsetof(cube::Creature, gapAC8) == 0xac8);
    std::memcpy(reinterpret_cast<unsigned char *>(creature) + 0xb10, &position, sizeof(position));
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
static void objectiveMarker(cube::Game *game, const EventRecord &saved, const Active &active) {
    unsigned index = chestEvent(saved.kind) ? Container : Npc;
    if (saved.kind == Kind::Ambush) {
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
        if (!saved.rewardClaimed && !saved.failed)
            if (auto actor = native::resident(*world, active.actors[index]))
                actor->entity_data.binary_toggles |= MapMarker;
    }
}
static bool spawn(Data &value, cube::Game *game, EventRecord &saved,
                  const std::array<LongVector3, ActorCount> &points, Active *existing = nullptr) {
    Active active{{saved.world, saved.region, saved.slot}};
    if (existing)
        active = *existing;
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
    if (!chestEvent(saved.kind) && saved.kind != Kind::Ambush && missing(Npc) && captiveReady) {
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
        if (guardianEvent(saved.kind))
            unregisterGuardian(value, active.actors[enemy]);
        auto creature = spawnActor(value, active, enemy, points[enemy], saved.enemyRaces[enemy],
                                   HostileBehaviour, saved.enemyStars[enemy]);
        if (!creature)
            continue;
        creature->entity_data.appearance.flags2 &= ~(Friendly | Named | Mini | Boss);
        native::allowRoaming(creature);
        if (guardianEvent(saved.kind)) {
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
        }
    }
    if (chestEvent(saved.kind) && missing(Container)) {
        auto chest = spawnActor(value, active, Container, points[Object], value.assets.chestRace,
                                objects::ExamineBehaviour, 0);
        if (chest) {
            noncombatObject(chest);
            chest->entity_data.interaction_state = saved.rewardClaimed ? 2 : 1;
        }
    }
    bool complete = saved.kind == Kind::Ambush ? active.actors[0] != 0
                    : chestEvent(saved.kind)   ? active.actors[Container] != 0
                                               : active.actors[Npc] != 0;
    complete &= saved.kind != Kind::Battle || active.actors[Companion] != 0;
    complete &= saved.kind != Kind::ChestSack || active.actors[Object] != 0;
    complete &= saved.kind != Kind::Rescue || saved.rewardClaimed || active.actors[Object] != 0;
    for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy)
        complete &= (saved.defeatedMask & (1u << enemy)) || active.actors[enemy] != 0;
    if (captiveNeedsCage(saved.kind, saved.rewardClaimed))
        bindRescue(game, active);
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
static void retire(Data &value, cube::Game *game, Active &active) {
    auto &saved = record(value, active.key);
    active.retiring = true; // Death callbacks must not complete an unloaded event.
    for (unsigned index = 0; index < ActorCount; ++index) {
        if (auto creature = native::resident(game->host.world, active.actors[index])) {
            if (index < saved.enemyCount && !(saved.defeatedMask & (1u << index)))
                saved.enemyHPFraction[index] = fraction(creature);
            if (index == Npc)
                saved.npcHPFraction = fraction(creature);
        }
        // Client replicas are closed too: a retired chest must never reopen.
        despawn(game, active.actors[index]);
    }
    value.dirty = true;
}
static void death(cube::Game *game, cube::Creature *creature);
// Event actors that no instance owns (left from a dropped or older instance)
// are removed: they could duplicate a role or open as an ordinary chest.
static void removeOrphans(const Data &value, cube::Game *game) {
    std::vector<uint64_t> orphans;
    for (auto creature : game->host.world.creatures) {
        if (!creature || creature->entity_data.HP <= 0)
            continue;
        auto id = uint64_t(creature->id);
        if (eventEntityId(id) && !owned(value, id))
            orphans.push_back(id);
    }
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
    for (auto it = value.active.begin(); it != value.active.end();) {
        if (it->retiring && std::none_of(it->actors.begin(), it->actors.end(),
                                         [&](auto id) { return resident(game, id) != nullptr; })) {
            unregisterGuardian(value, it->actors[0]);
            it = value.active.erase(it);
            continue;
        }
        auto &saved = record(value, it->key);
        if (saved.rewardClaimed || saved.failed) {
            if (!it->finishAt)
                it->finishAt = GetTickCount64() + 10000;
            if (!it->retiring && GetTickCount64() >= it->finishAt)
                retire(value, game, *it);
        }
        if (!it->retiring)
            for (unsigned index = 0; index <= Npc; ++index)
                if (auto creature = native::resident(game->host.world, it->actors[index])) {
                    if (creature->entity_data.HP <= 0)
                        death(game, creature);
                    else if (index < saved.enemyCount)
                        saved.enemyHPFraction[index] = fraction(creature);
                    else if (index == Npc && saved.kind == Kind::Battle)
                        saved.npcHPFraction = fraction(creature);
                }
        if (!it->retiring && !saved.rewardClaimed && !saved.failed) {
            if (captiveNeedsCage(saved.kind, saved.rewardClaimed)) {
                // If the cage unloaded or failed, the captive goes too; both are
                // recreated together, the captive inside the new cage.
                if (!resident(game, it->actors[Object]) && it->actors[Npc])
                    releaseRole(game, *it, Npc);
                bindRescue(game, *it);
            }
            if (within(player, saved.site, 192)) {
                // A chunk may unload actors. Retain this instance and recreate
                // only absent roles when it is approached again.
                bool missing =
                    saved.kind != Kind::Ambush &&
                    !resident(game, it->actors[chestEvent(saved.kind) ? Container : Npc]);
                for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy)
                    missing |=
                        !(saved.defeatedMask & (1u << enemy)) && !resident(game, it->actors[enemy]);
                missing |= saved.kind == Kind::Battle && !resident(game, it->actors[Companion]);
                missing |= (saved.kind == Kind::Rescue || saved.kind == Kind::ChestSack) &&
                           !resident(game, it->actors[Object]);
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
        }
        objectiveMarker(game, saved, *it);
        ++it;
    }
}
static void testEncounter(Data &value, cube::Game *game, Site player, Region region) {
    if (value.requestedTest == Kind::Count)
        return;
    auto say = [&](const wchar_t *text) {
        value.bridge.dialogue(game, game->world->local_creature, text);
    };
    if (value.active.size() >= value.cap) {
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
        value.active.clear();
        value.referrals.clear();
        for (auto &entry : value.guardianIds)
            entry.store(0, std::memory_order_relaxed);
        value.requestedTest = Kind::Count;
        return;
    }
    auto nowTick = GetTickCount64();
    if (nowTick < value.nextTick)
        return;
    value.nextTick = nowTick + 1000;
    auto identity = worldIdentity(game->host.world);
    bool newSession = value.session != &game->host.world || value.world != identity;
    if (newSession) {
        value.active.clear();
        value.referrals.clear();
        for (auto &entry : value.guardianIds)
            entry.store(0, std::memory_order_relaxed);
        value.session = &game->host.world;
        value.world = identity;
        value.assets = {};
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
                    if (testing || value.active.size() + pending.size() >= value.cap)
                        continue;
                    if (exists)
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
    }
    // Construct native actors only after releasing terrain locks. Appearance,
    // dialogue and host registration can enter other engine subsystems.
    for (const auto &request : pending)
        spawn(value, game, record(value, request.key), request.points);
    flush(value);
}
static void death(cube::Game *game, cube::Creature *creature) {
    if (!game || !creature)
        return;
    auto &value = data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    for (auto &active : value.active) {
        if (active.retiring)
            continue;
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
static void describeReferral(Data &value, cube::Game *game, cube::Creature *speaker,
                             const EventRecord &saved) {
    auto origin = native::position(speaker);
    auto distance = std::hypot(double(saved.site.x - origin.x), double(saved.site.y - origin.y));
    const wchar_t *request = saved.kind == Kind::Rescue   ? L"Free the trapped adventurer"
                             : saved.kind == Kind::Battle ? L"Help the adventurers in battle"
                                                          : L"Defeat the enemy group";
    wchar_t message[256];
    swprintf_s(message,
               L"%s about %.0f blocks from here. The objective area is marked on your map. "
               L"Return to town and speak to us afterward for your reward.",
               request, distance);
    value.bridge.dialogue(game, speaker, message);
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
            say(L"Thank you for helping. Safe travels!");
        else if (saved.failed)
            say(L"That encounter is over. We will have more news another time.");
        else if (!saved.objectiveDone)
            describeReferral(value, game, creature, saved);
        else {
            auto active =
                std::find_if(value.active.begin(), value.active.end(), [&](const Active &entry) {
                    return entry.key == it->objective && !entry.retiring;
                });
            if (active != value.active.end() && deliver(value, game, saved, *active)) {
                if (saved.kind == Kind::Rescue)
                    freeRescue(game, *active);
                objectiveMarker(game, saved, *active);
                say(L"Thank you. This equipment is your reward.");
            } else
                say(L"Your reward is not ready. Please speak to me again in a moment.");
        }
        return 1;
    }
    // A town may direct the player to an existing instance instead of spawning
    // a second copy of it. The shared completion flag prevents double rewards.
    for (const auto &active : value.active) {
        const auto &saved = record(value, active.key);
        if (active.retiring || saved.failed || saved.rewardClaimed ||
            !sameRegion(saved.region, region) || !town::referralKind(saved.kind))
            continue;
        value.referrals.push_back({center, active.key, saved.cycle});
        describeReferral(value, game, creature, saved);
        return 1;
    }
    if (value.active.size() >= value.cap)
        return 0;
    auto selected = unusedSlot(value, region);
    if (!selected)
        return 0;
    auto candidate = *selected;
    auto roll = plan(value.world, region, candidate.slot, candidate.cycle).roll;
    candidate.kind = town::kind(mix(roll ^ uint64_t(creature->id)));
    candidate.enemyCount = enemyCount(candidate.kind, roll);
    if (!available(value.assets, candidate.kind))
        return 0;
    std::array<LongVector3, ActorCount> points{};
    bool placed = false;
    {
        native::ZoneLock locked(game->world);
        if (!locked.held)
            return 0;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            candidate.site = town::candidate(center, village.radius, attempt);
            if (!relocating(candidate.site, candidate.previousSite, candidate.cycle) ||
                !siteAvailable(value.state, {candidate.world, candidate.region, candidate.slot},
                               candidate.site, utc()) ||
                !place(game, candidate, points))
                continue;
            placed = chooseEnemies(game, region, value, roll, candidate);
            break;
        }
    }
    if (!placed)
        return 0; // No safe loaded site: preserve this villager's ordinary talk.
    candidate.planned = true;
    *selected = candidate;
    value.dirty = true;
    spawn(value, game, *selected, points);
    value.referrals.push_back(
        {center, {candidate.world, candidate.region, candidate.slot}, candidate.cycle});
    describeReferral(value, game, creature, *selected);
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
        // A retiring instance still owns its actors: handled with no effect, so
        // its chest never falls through to CubeMega's ordinary chest loot.
        if (active.retiring)
            return 1;
        auto &saved = record(value, active.key);
        auto speaker = creature;
        if (saved.kind == Kind::Rescue && actor == Object)
            if (auto npc = resident(game, active.actors[Npc]))
                speaker = npc;
        auto say = [&](const wchar_t *message) { value.bridge.dialogue(game, speaker, message); };
        if (saved.failed)
            return 1;
        if (saved.rewardClaimed) {
            say(chestEvent(saved.kind) ? L"The chest has been emptied."
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
    if (value.enabled && setting(L"ReplaceTravelLocations", 1)) {
        std::string error;
        if (!travel::initialize(&error))
            log(error.c_str());
        else
            log("Original glider/boat/reins/boots sites now generate native dungeons.");
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
} // namespace eventmod

class OverworldEvents : public GenericMod {
  public:
    void Initialize() override {
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
EXPORT int IsOverworldEventEntity(uint64_t id) {
    auto &value = eventmod::data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    return eventmod::owned(value, id) ? 1 : 0;
}
