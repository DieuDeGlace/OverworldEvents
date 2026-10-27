#include "../src/RuntimeAssets.h"
#include <cassert>
#include <map>
#include <cstdio>

struct SpeechFixture {
    std::map<int32_t, std::wstring> entity_type_id_map;
    std::map<std::pair<uint32_t, uint32_t>, std::wstring> item_identifier_map;
};
int main() {
    using namespace encounters;
    SpeechFixture speech;
    assert(!resolveAssets(speech).ready);
    // Identifiers and categories checked against Steam's live speech maps.
    speech.entity_type_id_map = {{10, L"ElfMale"},    {12, L"HumanMale"}, {46, L"Ogre"},
                                 {181, L"WoodChest"}, {194, L"Cage1"},    {242, L"Sack"}};
    speech.item_identifier_map = {
        {{22, 0}, L"GoldKey"}, {{22, 6}, L"RustyKey"}, {{24, 13}, L"GateKey"}};
    auto assets = resolveAssets(speech);
    assert(assets.ready && assets.cageRace == 194 && assets.sackRace == 242);
    assert(assets.chestRace == 181 && assets.ogreRace == 46 && assets.npcRaces.size() == 2);
    assert(assets.keyCategory == 22 && assets.keyId == 6 && assets.keyName == L"RustyKey");
    // Localized/partial names and a similarly named travel special are not keys.
    speech.item_identifier_map = {
        {{24, 6}, L"RustyKey"}, {{22, 9}, L"CopperKeyExtra"}, {{24, 13}, L"GateKey"}};
    assert(!resolveAssets(speech).keyId);
    speech.item_identifier_map = {
        {{22, 6}, L"RustyKey"}, {{22, 7}, L"RustyKey"}, {{22, 9}, L"CopperKey"}};
    assets = resolveAssets(speech);
    assert(assets.keyId == 9); // Reject ambiguous RustyKey, use an exact safe variant.
    speech.entity_type_id_map.emplace(195, L"Cage1");
    assert(!resolveAssets(speech).cageRace);
    speech.entity_type_id_map.emplace(107, L"SeaCrab");
    speech.entity_type_id_map.emplace(153, L"MawFish");
    speech.entity_type_id_map.emplace(154, L"Piranha");
    speech.entity_type_id_map.emplace(106, L"Crab");
    speech.entity_type_id_map.emplace(69, L"PlantCreature");
    assets = resolveAssets(speech);
    assert(aquaticRace(assets, 107) && aquaticRace(assets, 153) && aquaticRace(assets, 154));
    assert(!aquaticRace(assets, 106) && !aquaticRace(assets, 69));
    assert(!aquaticRace(assets, 70) && !aquaticRace(Assets{}, 153));
    puts("Native category-22 keys, exact identifiers and unavailable/ambiguous assets: PASS");
}
