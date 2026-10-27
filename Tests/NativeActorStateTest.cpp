#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/NativeWorld.h"
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    // A native-layout byte fixture: do not construct an SDK creature (its
    // allocator/constructor belongs to the game). Offsets are corroborated by
    // Steam AIManager::idle and World::spawn in the release reference.
    alignas(cube::Creature) std::array<unsigned char, sizeof(cube::Creature)> memory{};
    const int64_t position[3] = {-8417831200763LL, -1040357482993LL, 60072};
    std::memcpy(memory.data() + 0x10, position, sizeof(position));
    uint32_t behaviours = 0x80 | 0x04 | 0x100; // Follow, stand and rest/heal.
    std::memcpy(memory.data() + 0xbcc, &behaviours, sizeof(behaviours));
    const auto before = memory;
    auto creature = reinterpret_cast<cube::Creature *>(memory.data());
    encounters::native::allowRoaming(creature);
    std::memcpy(&behaviours, memory.data() + 0xbcc, sizeof(behaviours));
    assert(behaviours == (0x10 | 0x04 | 0x100)); // Vanilla wander, other modes kept.
    assert(std::memcmp(memory.data() + 0x1b8, position, sizeof(position)) == 0);
    // Combat, animation, identity, position and all unrelated native data stay intact.
    for (size_t i = 0; i < memory.size(); ++i)
        if (!(i >= 0x1b8 && i < 0x1d0) && !(i >= 0xbcc && i < 0xbd0))
            assert(memory[i] == before[i]);
    const auto initialized = memory;
    encounters::native::allowRoaming(creature);
    assert(memory == initialized);
    const LongVector3 anchor = creature->entity_data.position;
    const float smoothing = 120.f;
    std::memcpy(memory.data() + 0xb54, &smoothing, sizeof(smoothing));
    encounters::native::setStationaryPosition(creature, anchor);
    for (size_t offset : {size_t(0x10), size_t(0xaf8), size_t(0xb10)})
        assert(std::memcmp(memory.data() + offset, position, sizeof(position)) == 0);
    float remaining;
    std::memcpy(&remaining, memory.data() + 0xb54, sizeof(remaining));
    assert(remaining == 0.f);
    assert(std::memcmp(memory.data() + 0x1b8, position, sizeof(position)) == 0);
    puts("Native home/leash and wander initialization; unrelated fields preserved: PASS");
}
