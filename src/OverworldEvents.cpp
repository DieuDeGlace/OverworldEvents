#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "EventRules.h"
#include "NativeWorld.h"
#include "RuntimeAssets.h"
#include "StateStore.h"
#include "TravelLocations.h"
#include <array>
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
constexpr unsigned Npc = 6, Companion = 7, Object = 8, Container = 9;
constexpr unsigned ActorCount = 10;
struct Bridge {
    cube::Creature *(*spawn)(const LongVector3 *, const IntVector2 *, int, int, int) = nullptr;
    void (*drop)(cube::Game *, cube::Creature *, const cube::Item *, cube::Creature *) = nullptr;
    int (*gear)(const IntVector2 *, uint64_t, cube::Item *) = nullptr;
    void (*dialogue)(cube::Game *, cube::Creature *, const wchar_t *) = nullptr;
    int (*grantKey)(cube::Game *, cube::Creature *, const cube::Item *) = nullptr;
    int (*consumeKey)(cube::Game *, cube::Creature *, const cube::Item *) = nullptr;
};
struct Active {
    EventKey key;
    std::array<uint64_t, ActorCount> actors{};
    bool retiring = false;
};
struct SpawnRequest {
    EventKey key;
    std::array<LongVector3, ActorCount> points;
};
struct Data {
    std::recursive_mutex gate;
    StateStore state;
    std::vector<Active> active;
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
    if (!version || version() != 1)
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
    for (const auto &active : value.active)
        for (auto actor : active.actors)
            if (actor == id)
                return true;
    return false;
}
static float fraction(cube::Creature *creature) {
    auto maximum = native::maximumHP(creature);
    return std::isfinite(maximum) && maximum > 0
               ? std::clamp(creature->entity_data.HP / maximum, 0.f, 1.f)
               : 1.f;
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
        if (entity.hostility_type != 1 || entity.HP <= 0 || entity.race == 276 ||
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
static bool available(const Assets &assets, Kind kind) {
    if (!chestEvent(kind) && assets.npcRaces.empty())
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
static bool place(cube::Game *game, EventRecord &saved,
                  std::array<LongVector3, ActorCount> &points) {
    int center = 0;
    if (!native::overworld(game->world, saved.site, saved.region) ||
        !footprint(game->world, saved.site, center, saved.kind == Kind::Rescue ? 3 : 1,
                   saved.kind == Kind::Rescue))
        return false;
    for (unsigned actor = 0; actor < ActorCount; ++actor) {
        auto offset = offsets[actor];
        // The native hostile/NPC aggro radius is 20 blocks. A guardian starts
        // farther from the quest giver/prisoner, who stays at the event center.
        if (actor == 0 && saved.kind != Kind::Battle)
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
    entity.HP = native::maximumHP(creature);
}
static bool spawn(Data &value, cube::Game *game, EventRecord &saved,
                  const std::array<LongVector3, ActorCount> &points) {
    Active active{{saved.world, saved.region, saved.slot}};
    auto roll = plan(saved.world, saved.region, saved.slot, saved.cycle).roll;
    int friendlyRace = value.assets.npcRaces.empty()
                           ? -1
                           : value.assets.npcRaces[roll % value.assets.npcRaces.size()];
    if (!chestEvent(saved.kind)) {
        // The native behaviour-3 talk branch reaches CubeMega's existing hook.
        // Behaviour 4 follows a separate path and cannot deliver event dialogue.
        auto npc = spawnActor(value, active, Npc, points[Npc], friendlyRace, 3, 2);
        if (!npc)
            return false;
        npc->entity_data.appearance.flags2 =
            (npc->entity_data.appearance.flags2 & ~(NoTalk | CanPickup | Petrified)) | Friendly;
        npc->entity_data.HP = native::maximumHP(npc) * std::max(.05f, saved.npcHPFraction);
        snprintf(npc->entity_data.name, 16, "%s", "Adventurer");
        if (saved.kind == Kind::Battle)
            native::allowRoaming(npc);
    }
    if (saved.kind == Kind::Battle) {
        auto ally = spawnActor(value, active, Companion, points[Companion], friendlyRace, 3, 1);
        if (ally) {
            ally->entity_data.appearance.flags2 =
                (ally->entity_data.appearance.flags2 & ~(NoTalk | CanPickup | Petrified)) |
                Friendly;
            native::allowRoaming(ally);
        }
    }
    for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy) {
        if (saved.defeatedMask & (1u << enemy))
            continue;
        auto creature = spawnActor(value, active, enemy, points[enemy], saved.enemyRaces[enemy], 1,
                                   saved.enemyStars[enemy]);
        if (!creature)
            continue;
        native::allowRoaming(creature);
        if (saved.kind != Kind::Battle)
            nameBoss(creature, roll);
        creature->entity_data.HP =
            native::maximumHP(creature) * std::clamp(saved.enemyHPFraction[enemy], .01f, 1.f);
    }
    if (saved.kind == Kind::Rescue && !saved.objectiveDone)
        spawnActor(value, active, Object, points[Object], *value.assets.cageRace, 7, 0);
    if (saved.kind == Kind::ChestSack) {
        auto sack =
            spawnActor(value, active, Object, points[Container], *value.assets.sackRace, 3, 0);
        if (sack)
            sack->entity_data.appearance.flags2 =
                (sack->entity_data.appearance.flags2 & ~(NoTalk | CanPickup | Petrified)) |
                Friendly;
    }
    if (chestEvent(saved.kind)) {
        auto chest =
            spawnActor(value, active, Container, points[Object], value.assets.chestRace, 8, 0);
        if (chest) {
            chest->entity_data.interaction_state = saved.rewardClaimed ? 2 : 1;
            chest->entity_data.binary_toggles &= ~(1 << 8);
        }
    }
    bool complete =
        chestEvent(saved.kind) ? active.actors[Container] != 0 : active.actors[Npc] != 0;
    complete &= saved.kind != Kind::Battle || active.actors[Companion] != 0;
    complete &= saved.kind != Kind::ChestSack || active.actors[Object] != 0;
    complete &= saved.kind != Kind::Rescue || saved.objectiveDone || active.actors[Object] != 0;
    for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy)
        complete &= (saved.defeatedMask & (1u << enemy)) || active.actors[enemy] != 0;
    if (!complete) {
        active.retiring = true;
        for (auto id : active.actors)
            if (auto actor = native::resident(game->host.world, id)) {
                actor->entity_data.hostility_type = 7;
                actor->entity_data.HP = 0;
            }
        log("Incomplete native encounter spawn retired; its plan is retained for retry.");
    }
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
        auto creature = native::resident(game->host.world, active.actors[index]);
        if (!creature)
            continue;
        if (index < saved.enemyCount && !(saved.defeatedMask & (1u << index)))
            saved.enemyHPFraction[index] = fraction(creature);
        if (index == Npc)
            saved.npcHPFraction = fraction(creature);
        creature->entity_data.hostility_type = 7;
        creature->entity_data.HP = 0;
    }
    value.dirty = true;
}
static void death(cube::Game *game, cube::Creature *creature);
static void update(Data &value, cube::Game *game, Site player) {
    for (auto it = value.active.begin(); it != value.active.end();) {
        bool any = false;
        for (auto id : it->actors)
            any |= native::resident(game->host.world, id) != nullptr;
        if (!any) {
            it = value.active.erase(it);
            continue;
        }
        auto &saved = record(value, it->key);
        if (!it->retiring)
            for (unsigned index = 0; index <= Npc; ++index)
                if (auto creature = native::resident(game->host.world, it->actors[index]))
                    if (creature->entity_data.HP <= 0)
                        death(game, creature);
        if (!it->retiring && !within(player, saved.site, 256))
            retire(value, game, *it);
        if (!it->retiring && saved.kind == Kind::Rescue && saved.objectiveDone) {
            auto cage = native::resident(game->host.world, it->actors[Object]);
            if (cage) {
                cage->entity_data.HP = 0;
                it->actors[Object] = 0;
            }
        }
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
        say(L"Event limit reached. Move away from the active encounters before testing another.");
        value.requestedTest = Kind::Count;
        return;
    }
    if (!available(value.assets, value.requestedTest)) {
        say(L"The native assets needed for that event are unavailable.");
        value.requestedTest = Kind::Count;
        return;
    }
    EventRecord *selected = nullptr;
    for (unsigned slot = 0; slot < value.slots; ++slot) {
        auto &saved = value.state.get(value.world, region, slot);
        if (!saved.planned && !saved.rewardClaimed && !saved.failed) {
            selected = &saved;
            break;
        }
    }
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
        if (!relocating(candidate.site, candidate.previousSite, candidate.cycle))
            continue;
        std::array<LongVector3, ActorCount> points;
        {
            native::ZoneLock locked(game->world);
            if (!locked.held)
                return;
            if (!place(game, candidate, points))
                continue;
            if (candidate.enemyCount) {
                auto pool = enemies(game, region, value);
                if (pool.empty())
                    continue;
                for (unsigned index = 0; index < candidate.enemyCount; ++index) {
                    auto pick = pool[mix(roll + index) % pool.size()];
                    candidate.enemyRaces[index] = pick->entity_data.race;
                    candidate.enemyStars[index] =
                        stars(int(pick->entity_data.level), candidate.kind != Kind::Battle);
                }
            }
        }
        candidate.planned = true;
        *selected = candidate;
        value.dirty = true;
        bool started = spawn(value, game, *selected, points);
        wchar_t message[180];
        swprintf_s(message,
                   L"%s Event site: %lld, %lld. Progress follows the normal cooldown rules.",
                   started ? L"Test encounter started." : L"Native spawn needs a retry.",
                   candidate.site.x, candidate.site.y);
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
    if (!value.enabled || !game || !game->host.running || !game->world ||
        !game->world->local_creature) {
        if (value.loaded)
            flush(value);
        value.session = nullptr;
        value.active.clear();
        value.requestedTest = Kind::Count;
        return;
    }
    auto nowTick = GetTickCount64();
    if (nowTick < value.nextTick)
        return;
    value.nextTick = nowTick + 1000;
    auto identity = worldIdentity(game->host.world);
    if (value.session != &game->host.world || value.world != identity) {
        value.active.clear();
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
                    std::array<LongVector3, ActorCount> points;
                    if (!place(game, saved, points)) {
                        if (!saved.planned)
                            nextCandidate(saved);
                        continue;
                    }
                    if (!saved.planned && saved.enemyCount) {
                        auto pool = enemies(game, area, value);
                        if (pool.empty())
                            continue;
                        for (unsigned enemy = 0; enemy < saved.enemyCount; ++enemy) {
                            auto pick = pool[mix(planned.roll + enemy) % pool.size()];
                            saved.enemyRaces[enemy] = pick->entity_data.race;
                            saved.enemyStars[enemy] =
                                stars(int(pick->entity_data.level), saved.kind != Kind::Battle);
                        }
                    }
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
static bool deliver(Data &value, cube::Game *game, EventRecord &saved, const Active &active) {
    cube::Item reward;
    IntVector2 area(saved.region.x, saved.region.y);
    auto roll = plan(saved.world, saved.region, saved.slot, saved.cycle).roll;
    if (!value.bridge.gear(&area, roll, &reward))
        return false;
    saved.rewardClaimed = true;
    saved.deadlineUTCsecs = cooldownDeadline(utc(), value.cooldown);
    saved.emptySince = 0;
    value.dirty = true;
    if (!flush(value)) {
        saved.rewardClaimed = false;
        saved.deadlineUTCsecs = 0;
        return false;
    }
    // Resolve a surviving corpse at delivery; no native pointers survive frames.
    auto source = native::resident(game->host.world, active.actors[0]);
    value.bridge.drop(game, game->world->local_creature, &reward, source);
    return true;
}
static int interaction(cube::Game *game, cube::Creature *creature) {
    if (!game || !game->world || !game->world->local_creature || !creature)
        return 0;
    auto &value = data();
    std::lock_guard<std::recursive_mutex> guard(value.gate);
    for (auto &active : value.active) {
        if (active.retiring)
            continue;
        unsigned actor = ActorCount;
        for (unsigned index = Npc; index < ActorCount; ++index)
            if (active.actors[index] == uint64_t(creature->id))
                actor = index;
        if (actor == ActorCount)
            continue;
        auto &saved = record(value, active.key);
        auto say = [&](const wchar_t *message) { value.bridge.dialogue(game, creature, message); };
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
                if (auto host = native::resident(game->host.world, active.actors[Container]))
                    host->entity_data.interaction_state = 2;
            };
            if (saved.objectiveDone) {
                if (deliver(value, game, saved, active)) {
                    open();
                    say(L"You found equipment inside the chest.");
                }
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
            }
            return 1;
        }
        if (actor != Npc) {
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
        if (deliver(value, game, saved, active))
            say(L"Thank you. This equipment is your reward.");
        return 1;
    }
    return 0;
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
    for (unsigned index = 0; index < unsigned(Kind::Count); ++index)
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
