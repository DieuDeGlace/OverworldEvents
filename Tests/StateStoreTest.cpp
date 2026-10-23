#include "../src/StateStore.h"
#include <Windows.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

using namespace encounters;
namespace {
std::string read(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    assert(file.good());
}
std::string changeField(const std::string &text, size_t field, const char *value) {
    auto start = text.find("E ");
    auto finish = text.find('\n', start);
    std::istringstream stream(text.substr(start, finish - start));
    std::vector<std::string> fields;
    std::string token;
    while (stream >> token)
        fields.push_back(token);
    assert(field < fields.size());
    fields[field] = value;
    std::ostringstream replacement;
    for (size_t i = 0; i < fields.size(); ++i)
        replacement << (i ? " " : "") << fields[i];
    auto result = text;
    result.replace(start, finish - start, replacement.str());
    return result;
}
void same(const EventRecord &a, const EventRecord &b) {
    assert(a.world == b.world && a.region.x == b.region.x && a.region.y == b.region.y &&
           a.slot == b.slot && a.cycle == b.cycle && a.kind == b.kind && a.site.x == b.site.x &&
           a.site.y == b.site.y && a.previousSite.x == b.previousSite.x &&
           a.previousSite.y == b.previousSite.y && a.attempt == b.attempt &&
           a.enemyCount == b.enemyCount && a.enemyRaces == b.enemyRaces &&
           a.enemyStars == b.enemyStars && a.enemyHPFraction == b.enemyHPFraction &&
           a.npcHPFraction == b.npcHPFraction && a.defeatedMask == b.defeatedMask &&
           a.keyOwned == b.keyOwned && a.keySearchFound == b.keySearchFound &&
           a.objectiveDone == b.objectiveDone && a.rewardClaimed == b.rewardClaimed &&
           a.deadlineUTCsecs == b.deadlineUTCsecs && a.emptySince == b.emptySince &&
           a.planned == b.planned && a.failed == b.failed && a.townQuest == b.townQuest &&
           a.awaitingSite == b.awaitingSite && a.town.x == b.town.x && a.town.y == b.town.y &&
           a.difficultyLevel == b.difficultyLevel);
}
} // namespace

int main() {
    {
        // Landmark records: a leader plus 2-5 followers, saved and reloaded.
        StateStore landmarks;
        auto &hill = landmarks.get(42, {3, -4}, FirstLandmarkSlot + 5);
        hill.kind = Kind::Hill;
        hill.planned = true;
        hill.enemyCount = 5;
        hill.site = {24600, -32700};
        hill.previousSite = {24597, -32702};
        for (unsigned i = 0; i < 5; ++i)
            hill.enemyRaces[i] = 40 + i;
        assert(StateStore::valid(hill));
        auto shrine = hill;
        shrine.kind = Kind::Shrine;
        shrine.enemyCount = 3;
        shrine.enemyRaces[3] = shrine.enemyRaces[4] = 0;
        assert(StateStore::valid(shrine));
        shrine.enemyCount = 2;
        shrine.enemyRaces[2] = 0;
        assert(!StateStore::valid(shrine)); // A landmark always has followers.
        auto file = std::filesystem::temp_directory_path() /
                    (L"OverworldEvents-Landmark-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
        assert(landmarks.save(file));
        StateStore reloaded;
        assert(reloaded.load(file));
        auto *back = reloaded.find(42, {3, -4}, FirstLandmarkSlot + 5);
        assert(back && back->kind == Kind::Hill && back->enemyCount == 5 &&
               back->previousSite.x == 24597 && back->site.y == -32700);
        std::filesystem::remove(file);
    }
    auto directory = std::filesystem::temp_directory_path() /
                     (L"OverworldEvents-StateStoreTest-" + std::to_wstring(GetCurrentProcessId()) +
                      L"-" + std::to_wstring(GetTickCount64()));
    assert(std::filesystem::create_directory(directory));
    auto path = directory / L"state.txt";
    auto corrupt = directory / L"corrupt.txt";
    StateStore original;
    auto &record = original.get(0xfedcba9876543210ull, {-8, 13}, 2);
    record.cycle = 7;
    record.kind = Kind::ChestBoss;
    record.site = {-7500, 13500};
    record.previousSite = {-8000, 13200};
    record.attempt = 11;
    record.enemyCount = 1;
    record.enemyRaces[0] = 85;
    record.enemyStars[0] = 4;
    record.enemyHPFraction[0] = 0.123456789f;
    record.npcHPFraction = 0.7f;
    record.defeatedMask = 1;
    record.keyOwned = true;
    record.keySearchFound = true;
    record.objectiveDone = true;
    record.rewardClaimed = true;
    record.deadlineUTCsecs = 1791799200;
    record.emptySince = 1791792060;
    record.planned = true;
    auto &failed = original.get(2, {1, 2}, 0);
    failed = record;
    failed.world = 2;
    failed.region = {1, 2};
    failed.slot = 0;
    failed.kind = Kind::Bounty;
    failed.rewardClaimed = false;
    failed.failed = true;
    failed.npcHPFraction = 0;
    original.get(1, {-8, 13}, 2).site = {8, -13};
    original.get(1, {-8, 13}, 3).site = {9, -13};
    original.get(1, {8, 13}, 2).site = {10, -13};
    std::string error;
    assert(original.save(path, &error) && error.empty());
    auto persisted = read(path);
    StateStore loaded;
    assert(loaded.load(path, &error) && error.empty());
    assert(loaded.size() == 5);
    same(record, *loaded.find(record.world, record.region, record.slot));
    same(failed, *loaded.find(2, {1, 2}, 0));
    assert(loaded.find(1, {-8, 13}, 2)->site.x == 8);
    assert(loaded.find(1, {-8, 13}, 3)->site.x == 9);
    assert(loaded.find(1, {8, 13}, 2)->site.x == 10);
    assert(!loaded.find(2, {-8, 13}, 2));
    auto windowsText = persisted;
    for (auto i = windowsText.size(); i-- > 0;)
        if (windowsText[i] == '\n')
            windowsText.insert(i, 1, '\r');
    write(corrupt, windowsText);
    assert(loaded.load(corrupt));
    same(record, *loaded.find(record.world, record.region, record.slot));

    // Serialization is stable; replace an existing complete file atomically.
    assert(loaded.save(path));
    assert(read(path) == persisted);
    auto &progress = loaded.get(record.world, record.region, record.slot);
    progress.cycle = 8;
    assert(loaded.save(path));
    StateStore next;
    assert(next.load(path));
    assert(next.find(record.world, record.region, record.slot)->cycle == 8);

    std::vector<std::string> invalid{
        "",
        "OverworldEventsState 2\nRecords 0\n",
        "OverworldEventsState 1\nRecords -1\n",
        "OverworldEventsState 1\nRecords 65537\n",
        persisted.substr(0, persisted.size() - 7),
        persisted + "unexpected\n",
        changeField(persisted, 6, std::to_string(unsigned(Kind::Count)).c_str()),
        changeField(persisted, 12, "7"),
        changeField(persisted, 13, "128"),
        changeField(persisted, 14, "2"),
        changeField(persisted, 1, "-1"),
        changeField(persisted, 7, "2147483648"),
        changeField(persisted, 4, "32"),
        changeField(persisted, 21, "4096"),
        changeField(persisted, 22, "5"),
        changeField(persisted, 23, "nan"),
        changeField(persisted, 23, "1.01"),
        changeField(persisted, 11, "65536"),
        changeField(persisted, 18, "253402300800"),
        changeField(persisted, 12, "1junk")};
    auto oneLine = persisted.substr(persisted.find("E "));
    oneLine = oneLine.substr(0, oneLine.find('\n') + 1);
    invalid.push_back("OverworldEventsState 1\nRecords 2\n" + oneLine + oneLine);
    for (const auto &text : invalid) {
        write(corrupt, text);
        assert(!loaded.load(corrupt, &error) && !error.empty());
        assert(loaded.size() == 5);
        assert(loaded.find(record.world, record.region, record.slot)->cycle == 8);
    }
    assert(!loaded.load(directory / L"missing.txt"));

    auto stableFile = read(path);
    progress.enemyCount = 7;
    assert(!loaded.save(path));
    assert(read(path) == stableFile);
    progress.enemyCount = 1;
    progress.defeatedMask = 2;
    assert(!loaded.save(path));
    assert(read(path) == stableFile);
    progress.defeatedMask = 1;
    progress.kind = Kind::Count;
    assert(!loaded.save(path));
    assert(read(path) == stableFile);
    progress.kind = Kind::ChestBoss;
    progress.npcHPFraction = std::numeric_limits<float>::infinity();
    assert(!loaded.save(path));
    assert(read(path) == stableFile);
    progress.npcHPFraction = 0.7f;
    progress.kind = Kind::Battle;
    assert(!loaded.save(path)); // A planned battle cannot contain only one enemy.
    progress.kind = Kind::Ambush;
    assert(!loaded.save(path)); // Town enemy groups share the four-to-six bound.
    progress.enemyCount = 4;
    assert(loaded.save(path));
    assert(next.load(path));
    assert(next.find(record.world, record.region, record.slot)->kind == Kind::Ambush);
    progress.kind = Kind::ChestSack;
    assert(!loaded.save(path)); // Sack events have no combat enemies.
    progress.kind = Kind::ChestBoss;
    assert(!loaded.save(directory / L"missing-directory" / L"state.txt"));
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        assert(entry.path().extension() == L".txt"); // No abandoned temporary files.

    StateStore empty;
    assert(empty.save(path) && next.load(path) && next.size() == 0);
    // The test only removes its individually named files and empty directory.
    std::filesystem::remove(path);
    std::filesystem::remove(corrupt);
    std::filesystem::remove(directory);
    std::cout << "Event state roundtrip, corruption rejection, and atomic replace passed.\n";
}
