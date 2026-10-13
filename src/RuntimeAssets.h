#pragma once
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace encounters {
struct Assets {
    bool ready = false;
    std::optional<int> cageRace, sackRace, ogreRace;
    // CubeMegaMod's native chest factory uses race 181 for the ordinary chest.
    int chestRace = 181;
    std::vector<int> npcRaces;
    std::optional<int> keyCategory, keyId;
    std::wstring keyName;
};

template <class Speech>
inline std::optional<int> entityAsset(const Speech &speech, const wchar_t *name) {
    std::optional<int> result;
    for (const auto &entry : speech.entity_type_id_map) {
        if (entry.first < 0 || entry.second != name)
            continue;
        // Ambiguous identifiers are unavailable rather than silently choosing a race.
        if (result && *result != entry.first)
            return std::nullopt;
        result = entry.first;
    }
    return result;
}

template <class Speech> inline Assets resolveAssets(const Speech &speech) {
    Assets assets;
    if (speech.entity_type_id_map.empty() || speech.item_identifier_map.empty())
        return assets;

    for (const wchar_t *name : {L"Cage1", L"Cage2", L"Cage3", L"Cage4"}) {
        assets.cageRace = entityAsset(speech, name);
        if (assets.cageRace)
            break;
    }
    assets.sackRace = entityAsset(speech, L"Sack");
    assets.ogreRace = entityAsset(speech, L"Ogre");
    for (const wchar_t *name : {L"WoodChest", L"Chest"}) {
        auto race = entityAsset(speech, name);
        // CubeMegaMod's verified chest interaction branch only handles 181–184.
        if (race && *race >= 181 && *race <= 184) {
            assets.chestRace = *race;
            break;
        }
    }

    // These are exact native dictionary identifiers, not translated display names.
    for (const wchar_t *name :
         {L"HumanMale", L"HumanFemale", L"ElfMale", L"ElfFemale", L"DwarfMale", L"DwarfFemale",
          L"GoblinMale", L"GoblinFemale", L"LizardmanMale", L"LizardmanFemale", L"FrogmanMale",
          L"FrogmanFemale", L"OrcMale", L"OrcFemale", L"UndeadMale", L"UndeadFemale"}) {
        auto race = entityAsset(speech, name);
        if (race)
            assets.npcRaces.push_back(*race);
    }

    // Prefer the generic key. A known native variant is acceptable when the game
    // omits that identifier. Match the entire identifier, never a substring "Key".
    for (const wchar_t *name :
         {L"Key",      L"RustyKey",   L"CopperKey",  L"BronzeKey",    L"IronKey",    L"SilverKey",
          L"GoldKey",  L"BoneKey",    L"SkullKey",   L"ScratchedKey", L"AdornedKey", L"ObsidianKey",
          L"SteelKey", L"TwistedKey", L"SmallKey",   L"HeavyKey",     L"MassiveKey", L"WoodKey",
          L"StoneKey", L"TinyKey",    L"DelicateKey"}) {
        std::optional<std::pair<uint32_t, uint32_t>> selected;
        bool ambiguous = false;
        for (const auto &entry : speech.item_identifier_map) {
            if (entry.second != name || entry.first.first != 22 || entry.first.second > 19)
                continue;
            if (selected && *selected != entry.first) {
                ambiguous = true;
                break;
            }
            selected = entry.first;
        }
        if (!selected || ambiguous)
            continue;
        assets.keyCategory = int(selected->first);
        assets.keyId = int(selected->second);
        assets.keyName = name;
        break;
    }
    // Individual optional assets still gate their corresponding event types.
    // A native cage race does not establish cage collision or a door animation API.
    assets.ready = true;
    return assets;
}
} // namespace encounters
