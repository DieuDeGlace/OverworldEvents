#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "TravelLocations.h"
#include "NativeWorld.h"
#include "TravelRules.h"
#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace encounters::travel {
namespace {
using Classify = int (*)(cube::World *, int, int, bool);
using Quest = int (*)(void *, int, int);
using Append = void (*)(void *, const void *);
Classify originalClassify = nullptr;
Quest originalQuest = nullptr;
Append originalAppend = nullptr;
bool installed = false;
thread_local unsigned originalDepth = 0;
std::atomic<uint64_t> revision{1};
std::array<int, 4> dungeonTypes{};

struct OriginalScope {
    OriginalScope() {
        ++originalDepth;
    }
    ~OriginalScope() {
        --originalDepth;
    }
};
bool readTravelItem(const void *record) {
    auto item = static_cast<const unsigned char *>(record);
    int id = 0;
    std::memcpy(&id, item + 4, sizeof(id));
    return isTravelItem(item[0], id);
}
struct RegionKey {
    cube::World *world;
    int seed, x, y;
    bool operator==(const RegionKey &other) const {
        return world == other.world && seed == other.seed && x == other.x && y == other.y;
    }
};
struct RegionHash {
    size_t operator()(const RegionKey &key) const {
        return size_t(mix(uintptr_t(key.world) ^ mix(uint32_t(key.seed)) ^ gridKey(key.x, key.y)));
    }
};
using Choices = std::unordered_map<uint64_t, int>;
std::mutex choicesMutex;
std::unordered_map<RegionKey, std::shared_ptr<const Choices>, RegionHash> choicesCache;

// The verified native planner has no vtable. Its constructor initializes 0xa8
// bytes and owns several native maps/vectors; always use its native destructor.
struct Planner {
    alignas(8) unsigned char storage[0xa8]{};
    bool constructed = false;
    Planner(cube::World *world, Region region) {
        IntVector2 owner(region.x, region.y);
        reinterpret_cast<void *(*)(void *, cube::World *, IntVector2 *)>(native::base() + 0x115df0)(
            storage, world, &owner);
        constructed = true;
    }
    ~Planner() {
        if (constructed)
            reinterpret_cast<void (*)(void *)>(native::base() + 0x82450)(storage);
    }
    Planner(const Planner &) = delete;
    Planner &operator=(const Planner &) = delete;
};
using NativeQuestMap = std::map<std::array<int, 2>, int>;
static_assert(sizeof(NativeQuestMap) == 16, "Native planner requires the release MSVC map ABI.");

std::shared_ptr<const Choices> choices(cube::World *world, Region owner) {
    RegionKey key{world, world->seed, owner.x, owner.y};
    {
        std::lock_guard lock(choicesMutex);
        auto found = choicesCache.find(key);
        if (found != choicesCache.end())
            return found->second;
    }
    // No cache mutex is held while the native planner recursively queries POIs.
    // The caller's OriginalScope makes all those queries see unmodified data.
    auto result = std::make_shared<Choices>();
    Planner planner(world, owner);
    const auto &quests = *reinterpret_cast<const NativeQuestMap *>(planner.storage + 0x10);
    if (quests.size() > 4096)
        return {};
    for (const auto &quest : quests)
        if (isTravelQuest(quest.second))
            result->emplace(gridKey(quest.first[0], quest.first[1]), quest.second);
    std::lock_guard lock(choicesMutex);
    if (choicesCache.size() >= 128)
        choicesCache.clear();
    return choicesCache.emplace(key, result).first->second;
}

struct CachedGrid {
    cube::World *world = nullptr;
    int seed = 0, x = 0, y = 0, type = 0;
    bool ocean = false;
    uint64_t revision = 0;
};
thread_local std::array<CachedGrid, 16> gridCache{};

int classify(cube::World *world, int x, int y, bool ocean) {
    if (originalDepth || !world)
        return originalClassify(world, x, y, ocean);
    int normalizedX = ocean ? int(floorDivide(x, 2) * 2) : x;
    int normalizedY = ocean ? int(floorDivide(y, 2) * 2) : y;
    auto epoch = revision.load(std::memory_order_acquire);
    auto &cached = gridCache[(uint32_t(normalizedX) * 31u ^ uint32_t(normalizedY)) & 15u];
    if (cached.world == world && cached.seed == world->seed && cached.x == normalizedX &&
        cached.y == normalizedY && cached.ocean == ocean && cached.revision == epoch)
        return cached.type;
    int type = originalClassify(world, normalizedX, normalizedY, ocean);
    if (type) {
        try {
            OriginalScope original;
            IntVector2 center{};
            reinterpret_cast<IntVector2 *(*)(cube::World *, IntVector2 *, int, int, bool)>(
                native::base() + 0x362a0)(world, &center, normalizedX, normalizedY, ocean);
            auto owner = native::region(world, {center.x, center.y});
            auto selected = choices(world, owner);
            if (selected && selected->find(gridKey(normalizedX, normalizedY)) != selected->end())
                type = dungeonTypes[dungeonIndex(
                    worldIdentity(uint32_t(world->seed), world->world_name), uint32_t(world->seed),
                    normalizedX, normalizedY)];
        } catch (...) {
            // Failed allocation/planning preserves the original native location.
        }
    }
    cached = {world, world->seed, normalizedX, normalizedY, type, ocean, epoch};
    return type;
}
int quest(void *planner, int x, int y) {
    auto value = originalQuest(planner, x, y);
    return !originalDepth && isTravelQuest(value) ? 0 : value;
}
__declspec(noinline) void append(void *items, const void *record) {
    auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    // These two verified native generation call sites create POI travel rewards
    // and extra ocean boats. Inventory, shops and player item drops stay intact.
    if (!originalDepth && record && isGenerationCaller(native::base(), caller) &&
        readTravelItem(record))
        return;
    originalAppend(items, record);
}

bool matches(uintptr_t address, const char *hex) {
    for (size_t i = 0; hex[i * 2]; ++i) {
        auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
        auto value = unsigned(digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]));
        if (*reinterpret_cast<const unsigned char *>(address + i) != value)
            return false;
    }
    return true;
}
void jump(unsigned char *out, uintptr_t destination) {
    const unsigned char prefix[6]{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(out, prefix, sizeof(prefix));
    std::memcpy(out + sizeof(prefix), &destination, sizeof(destination));
}
struct Hook {
    uintptr_t offset;
    const char *signature;
    size_t stolen;
    void *replacement;
    int relativeOffset = -1, relativeEnd = 0;
    unsigned char *trampoline = nullptr;
    std::array<unsigned char, 32> original{};
    DWORD protection = 0;
};
void release(std::array<Hook, 3> &hooks) {
    for (auto &hook : hooks)
        if (hook.trampoline) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook.trampoline = nullptr;
        }
}
bool prepare(Hook &hook) {
    auto entry = reinterpret_cast<unsigned char *>(native::base() + hook.offset);
    auto origin = uintptr_t(entry) & ~uintptr_t(65535);
    for (uintptr_t distance = 65536; distance < 0x70000000 && !hook.trampoline; distance += 65536)
        for (int direction : {1, -1}) {
            auto address = direction > 0 ? origin + distance : origin - distance;
            hook.trampoline = static_cast<unsigned char *>(VirtualAlloc(
                reinterpret_cast<void *>(address), 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (hook.trampoline)
                break;
        }
    if (!hook.trampoline)
        return false;
    std::memcpy(hook.original.data(), entry, hook.stolen);
    std::memcpy(hook.trampoline, entry, hook.stolen);
    if (hook.relativeOffset >= 0) {
        int32_t displacement;
        std::memcpy(&displacement, entry + hook.relativeOffset, sizeof(displacement));
        int32_t relocated = 0;
        if (!relocateRIP(uintptr_t(entry + hook.relativeEnd),
                         uintptr_t(hook.trampoline + hook.relativeEnd), displacement, relocated))
            return false;
        std::memcpy(hook.trampoline + hook.relativeOffset, &relocated, sizeof(relocated));
    }
    jump(hook.trampoline + hook.stolen, uintptr_t(entry + hook.stolen));
    DWORD previous;
    return VirtualProtect(hook.trampoline, 64, PAGE_EXECUTE_READ, &previous) &&
           FlushInstructionCache(GetCurrentProcess(), hook.trampoline, 64);
}
bool fail(std::string *error, const char *message) {
    if (error)
        *error = message;
    return false;
}
} // namespace

void clearCache() {
    std::lock_guard lock(choicesMutex);
    choicesCache.clear();
    revision.fetch_add(1, std::memory_order_release);
}
bool initialize(std::string *error) {
    if (error)
        error->clear();
    if (installed)
        return true;
    if (!native::supported())
        return fail(error, "Travel location replacement requires the supported Steam executable.");
    std::array<Hook, 3> hooks{{
        {0x2b99d0, "40555356574156488bec4883ec50", ClassifyStolen,
         reinterpret_cast<void *>(&classify)},
        {0x93480, "4883ec18488b0505584b004833c44889442408", QuestStolen,
         reinterpret_cast<void *>(&quest), QuestCookieOffset, QuestCookieEnd},
        {0x129c00, "48895c2408574883ec20488bd9488bfa", AppendStolen,
         reinterpret_cast<void *>(&append)},
    }};
    for (const auto &hook : hooks)
        if (!matches(native::base() + hook.offset, hook.signature))
            return fail(error,
                        "Travel generation hook differs or is already owned by another mod.");
    if (!matches(native::base() + 0x115df0,
                 "488bc45556574154415541564157488da828feffff4881eca0020000") ||
        !matches(native::base() + 0x82450, "48895c24104889742418574883ec20") ||
        !matches(native::base() + 0x362a0, "4883ec08458bd04c8bda4585c0751a4585c975"))
        return fail(error, "Native planner or POI coordinate adapter differs.");
    const uint32_t branches[]{0x16b430, 0x16b4c6, 0x16b3fa, 0x16b41e};
    for (size_t wanted = 0; wanted < dungeonTypes.size(); ++wanted) {
        dungeonTypes[wanted] = 0;
        for (int i = 0; i < 47; ++i)
            if (*reinterpret_cast<const uint32_t *>(native::base() + 0x16b7dc + i * 4) ==
                branches[wanted])
                dungeonTypes[wanted] = i + 1;
        if (!dungeonTypes[wanted])
            return fail(error, "Native dungeon name dispatch differs.");
    }
    for (auto &hook : hooks)
        if (!prepare(hook)) {
            release(hooks);
            return fail(error, "Cannot prepare a travel generation trampoline.");
        }
    size_t writable = 0;
    for (auto &hook : hooks) {
        if (!VirtualProtect(reinterpret_cast<void *>(native::base() + hook.offset), hook.stolen,
                            PAGE_EXECUTE_READWRITE, &hook.protection)) {
            for (size_t i = 0; i < writable; ++i) {
                DWORD unused;
                VirtualProtect(reinterpret_cast<void *>(native::base() + hooks[i].offset),
                               hooks[i].stolen, hooks[i].protection, &unused);
            }
            release(hooks);
            return fail(error, "Cannot make travel generation hooks writable.");
        }
        ++writable;
    }
    originalClassify = reinterpret_cast<Classify>(hooks[0].trampoline);
    originalQuest = reinterpret_cast<Quest>(hooks[1].trampoline);
    originalAppend = reinterpret_cast<Append>(hooks[2].trampoline);
    for (auto &hook : hooks) {
        std::array<unsigned char, 32> patch{};
        patch.fill(0x90);
        jump(patch.data(), reinterpret_cast<uintptr_t>(hook.replacement));
        auto entry = reinterpret_cast<void *>(native::base() + hook.offset);
        std::memcpy(entry, patch.data(), hook.stolen);
        DWORD unused;
        VirtualProtect(entry, hook.stolen, hook.protection, &unused);
        FlushInstructionCache(GetCurrentProcess(), entry, hook.stolen);
    }
    // Installed trampolines intentionally live as long as the loaded mod DLL.
    installed = true;
    return true;
}
} // namespace encounters::travel
