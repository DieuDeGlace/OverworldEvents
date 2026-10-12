#pragma once
#include "cwsdk.h"
#include "EventRules.h"
#include <cmath>
#include <climits>

namespace encounters::native {
inline uintptr_t base() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}
inline bool supported() {
    auto memory = reinterpret_cast<const unsigned char *>(base());
    if (!memory)
        return false;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(memory);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return false;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(memory + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt->FileHeader.TimeDateStamp == 0x5d93ab0d && nt->OptionalHeader.SizeOfImage == 0x5b7000;
}
struct Location {
    int type;
    unsigned char ocean, padding[3];
    int subtype, radius, x, y;
    float distanceSquared, influence;
};
static_assert(sizeof(Location) == 32);
inline bool coordinate(Site point) {
    return point.x >= INT_MIN && point.x <= INT_MAX && point.y >= INT_MIN && point.y <= INT_MAX;
}
inline Region region(cube::World *world, Site point) {
    IntVector2 value{};
    if (coordinate(point))
        reinterpret_cast<void (*)(cube::World *, IntVector2 *, int, int)>(base() + 0x2ada20)(
            world, &value, int(point.x), int(point.y));
    return {value.x, value.y};
}
inline Location location(cube::World *world, Site point) {
    Location value{};
    if (coordinate(point))
        reinterpret_cast<void (*)(cube::World *, int, int, Location *)>(base() + 0x35ce0)(
            world, int(point.x), int(point.y), &value);
    return value;
}
inline Site position(cube::Creature *creature) {
    return {floorDivide(creature->entity_data.position.x, 65536),
            floorDivide(creature->entity_data.position.y, 65536)};
}
inline cube::Creature *resident(cube::World &world, uint64_t id) {
    if (!id)
        return nullptr;
    auto found = world.id_to_creature_map.find(id);
    return found == world.id_to_creature_map.end() ? nullptr : found->second;
}
inline float maximumHP(cube::Creature *creature) {
    return reinterpret_cast<float (*)(cube::Creature *)>(base() + 0x5fac0)(creature);
}
struct ZoneLock {
    cube::World *world;
    bool held = false;
    explicit ZoneLock(cube::World *value) : world(value) {
        if (!world || !TryEnterCriticalSection(&world->zones_critical_section))
            return;
        if (!TryEnterCriticalSection(&world->zones_mesh_critical_section)) {
            LeaveCriticalSection(&world->zones_critical_section);
            return;
        }
        held = true;
    }
    ~ZoneLock() {
        if (held) {
            LeaveCriticalSection(&world->zones_mesh_critical_section);
            LeaveCriticalSection(&world->zones_critical_section);
        }
    }
};
inline cube::Field *field(cube::World *world, Site point) {
    auto zx = floorDivide(point.x, 64), zy = floorDivide(point.y, 64);
    if (zx < INT_MIN || zx > INT_MAX || zy < INT_MIN || zy > INT_MAX)
        return nullptr;
    auto found = world->zones.find(IntVector2(int(zx), int(zy)));
    if (found == world->zones.end() || !found->second)
        return nullptr;
    return &found->second->fields[(point.x - zx * 64) * 64 + (point.y - zy * 64)];
}
inline int blockType(cube::Field &column, int z) {
    auto index = int64_t(z) - column.base_z;
    return index >= 0 && uint64_t(index) < column.blocks.size()
               ? int(column.blocks[size_t(index)].type)
           : z < column.base_z ? int(cube::Block::Solid)
                               : int(cube::Block::Air);
}
inline bool ground(cube::World *world, Site point, int &height, int clearance = 4) {
    auto column = field(world, point);
    if (!column || !std::isfinite(column->field_8) || fabsf(column->field_8) > 100000)
        return false;
    int top = int(std::floor(column->field_8)) + 8;
    for (int z = top; z >= top - 32; --z) {
        auto type = blockType(*column, z);
        if (type != cube::Block::Solid && type != cube::Block::Grass &&
            type != cube::Block::Ground && type != cube::Block::Snow && type != cube::Block::Path)
            continue;
        bool open = true;
        for (int above = 1; above <= clearance; ++above)
            open &= blockType(*column, z + above) == cube::Block::Air;
        if (open) {
            height = z + 1;
            return true;
        }
    }
    return false;
}
inline bool overworld(cube::World *world, Site point, Region owner) {
    auto actual = region(world, point);
    if (actual.x != owner.x || actual.y != owner.y)
        return false;
    auto poi = location(world, point);
    // The nearest descriptor can exist well outside its footprint. Avoid its
    // full influence and an extra margin around the location's own radius.
    if (!std::isfinite(poi.influence) || !std::isfinite(poi.distanceSquared) || poi.influence > 0)
        return false;
    auto safeRadius = std::max(96, poi.radius + 48);
    return poi.distanceSquared > float(safeRadius) * safeRadius;
}
} // namespace encounters::native
