#include "../src/TravelRules.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <numeric>

using namespace encounters::travel;

namespace {
void rewardRestrictions() {
    for (int value = -1; value <= 24; ++value)
        assert(isTravelQuest(value) == (value >= 6 && value <= 9));
    constexpr uintptr_t base = 0x180000000ull;
    for (unsigned category = 0; category <= 25; ++category)
        for (int id = -1; id <= 64; ++id) {
            bool requested = category == 24 && (id == 1 || id == 2 || id == 3 || id == 18);
            assert(isTravelItem(category, id) == requested);
            assert(suppressGeneratedItem(category, id, base, base + 0x11ccf1) == requested);
            assert(suppressGeneratedItem(category, id, base, base + 0x3181aa) == requested);
            // Adjacent instructions and arbitrary shop/inventory/drop callers
            // cannot be mistaken for either verified native generation site.
            for (uintptr_t caller : {base - 1, base, base + 0x11ccf0, base + 0x11ccf2,
                                     base + 0x3181a9, base + 0x3181ab, base + 0x82580})
                assert(!suppressGeneratedItem(category, id, base, caller));
        }
    // Native quest types 10–13 grant other specials, which remain available.
    for (int value = 10; value <= 13; ++value)
        assert(!isTravelQuest(value));
    for (int id : {4, 5, 7, 8, 9, 12})
        assert(!isTravelItem(24, id));
}
void deterministicChoices() {
    auto world = worldIdentity(42, "Test World");
    assert(world == 0x42a9b3b1d02864d2ull);
    assert(dungeonIndex(world, 42, 0, 0) == 0);
    assert(dungeonIndex(world, 42, -1, 1) == 3);
    assert(dungeonIndex(world, 42, 12345, -54321) == 3);
    assert(dungeonIndex(worldIdentity(UINT32_MAX, "Ice"), UINT32_MAX, INT32_MIN, INT32_MAX) == 2);
    assert(gridKey(-1, 1) == 0xffffffff00000001ull);
    assert(gridKey(1, -1) == 0x00000001ffffffffull);
    auto secondWorld = worldIdentity(42, "Another World");
    assert(world != secondWorld && world != worldIdentity(43, "Test World"));
    unsigned differentWorld = 0, differentSeed = 0;
    std::array<unsigned, 4> distribution{};
    for (int x = -20; x <= 20; ++x)
        for (int y = -20; y <= 20; ++y) {
            auto choice = dungeonIndex(world, 42, x, y);
            assert(choice < 4 && choice == dungeonIndex(world, 42, x, y));
            ++distribution[choice];
            differentWorld += choice != dungeonIndex(secondWorld, 42, x, y);
            differentSeed += choice != dungeonIndex(world, 43, x, y);
        }
    assert(differentWorld > 1000 && differentSeed > 1000);
    for (auto count : distribution)
        assert(count > 300 && count < 550);
}
void trampolineAudit() {
    // Independent disassembly audit, 2026-10-12, Steam executable timestamp
    // 0x5d93ab0d / image size 0x5b7000. The private decrypted capture was used
    // only for these instruction lengths and boundaries; game bytecode is not
    // included in this test or distributed with the mod.
    // 2b99d0..2b99de: five PUSH instructions, MOV frame, SUB stack; no relative operand.
    const std::array<unsigned, 7> classifier{2, 1, 1, 1, 2, 3, 4};
    // 93480..93493: SUB stack, MOV RIP-relative cookie, XOR, MOV cookie to stack.
    const std::array<unsigned, 4> query{4, 7, 3, 5};
    // 129c00..129c10: MOV saved register, PUSH, SUB stack, two register MOVs.
    const std::array<unsigned, 5> append{5, 1, 4, 3, 3};
    assert(std::accumulate(classifier.begin(), classifier.end(), 0u) == ClassifyStolen);
    assert(std::accumulate(query.begin(), query.end(), 0u) == QuestStolen);
    assert(std::accumulate(append.begin(), append.end(), 0u) == AppendStolen);
    assert(0x2b99d0 + ClassifyStolen == 0x2b99de);
    assert(0x93480 + QuestStolen == 0x93493);
    assert(0x129c00 + AppendStolen == 0x129c10);
    assert(ClassifyStolen >= 14 && QuestStolen >= 14 && AppendStolen >= 14);
    assert(QuestCookieOffset == int(query[0]) + 3);
    assert(QuestCookieEnd == int(query[0] + query[1]));
    assert(QuestCookieOffset + sizeof(int32_t) == QuestCookieEnd);

    // Synthetic displacement values exercise relocation in either direction,
    // preserving the effective cookie address rather than comparing formulas.
    constexpr uintptr_t source = 0x180093480ull;
    for (auto destination : {source - 0x10000, source + 0x10000, source + 0x70000000})
        for (int32_t original : {int32_t(-12345), int32_t(0), int32_t(0x123456)}) {
            std::array<unsigned char, QuestStolen> fixture{};
            fixture.fill(0x90);
            std::memcpy(fixture.data() + QuestCookieOffset, &original, sizeof(original));
            auto copied = fixture;
            int32_t relocated = 0;
            assert(relocateRIP(source + QuestCookieEnd, destination + QuestCookieEnd, original,
                               relocated));
            std::memcpy(copied.data() + QuestCookieOffset, &relocated, sizeof(relocated));
            int32_t observed = 0;
            std::memcpy(&observed, copied.data() + QuestCookieOffset, sizeof(observed));
            assert(int64_t(source + QuestCookieEnd) + original ==
                   int64_t(destination + QuestCookieEnd) + observed);
            for (size_t i = 0; i < fixture.size(); ++i)
                if (i < size_t(QuestCookieOffset) || i >= size_t(QuestCookieEnd))
                    assert(copied[i] == fixture[i]);
        }
    int32_t unchanged = 17;
    assert(!relocateRIP(source, source + 0x80000001ull, 0, unchanged) && unchanged == 17);
    assert(!relocateRIP(source, source - 0x80000000ull, 0, unchanged) && unchanged == 17);
    assert(!relocateRIP(UINT64_MAX, source, 0, unchanged) && unchanged == 17);
    assert(relocateRIP(source, source, INT32_MIN, unchanged) && unchanged == INT32_MIN);
    assert(relocateRIP(source, source, INT32_MAX, unchanged) && unchanged == INT32_MAX);
}
} // namespace

int main() {
    rewardRestrictions();
    deterministicChoices();
    trampolineAudit();
    std::cout
        << "Travel reward restrictions, deterministic dungeons, and trampoline audit passed.\n";
}
