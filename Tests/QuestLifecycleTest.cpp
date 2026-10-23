#include "../src/StateStore.h"
#include "../src/TownRules.h"
#include "../src/DifficultyRules.h"
#include <Windows.h>
#include <cassert>
#include <fstream>
#include <sstream>

int main() {
    using namespace encounters;
    StateStore state;
    auto &quest = state.get(42, {-2, 3}, 1);
    quest.kind = Kind::Rescue;
    quest.enemyCount = 1;
    quest.planned = quest.townQuest = quest.awaitingSite = true;
    quest.site = {-14000, 26000};
    quest.town = {-14500, 26000};
    const auto origin = quest.site;
    quest.difficultyLevel = 80;
    // No enemy races or loaded terrain yet: the destination is still a valid,
    // persisted quest, so talking does not depend on outside-town chunks.
    assert(StateStore::valid(quest));
    auto file = std::filesystem::temp_directory_path() /
                (L"QuestLifecycle-" + std::to_wstring(GetCurrentProcessId()) + L".state");
    assert(state.save(file));
    StateStore reloaded;
    assert(reloaded.load(file));
    assert(reloaded.restartUnfinished(42) == 0);
    auto &back = reloaded.get(42, {-2, 3}, 1);
    assert(back.townQuest && back.awaitingSite && back.difficultyLevel == 80);
    assert(back.site.x == quest.site.x && back.town.x == quest.town.x);
    // Encounter activation preserves the accepted objective and health; a
    // later session cannot reroll the quest or duplicate its completion.
    back.awaitingSite = false;
    back.enemyRaces[0] = 85;
    back.enemyHPFraction[0] = .4f;
    assert(reloaded.save(file));
    assert(state.load(file) && state.restartUnfinished(42) == 0);
    assert(state.get(42, {-2, 3}, 1).enemyHPFraction[0] == .4f);
    back.objectiveDone = true;
    back.defeatedMask = 1;
    back.rewardClaimed = true;
    back.deadlineUTCsecs = 1000;
    assert(reloaded.save(file) && state.load(file));
    assert(state.get(42, {-2, 3}, 1).rewardClaimed);
    // Invalid quest metadata must not replace valid persisted state.
    back.awaitingSite = true;
    assert(!StateStore::valid(back) && !reloaded.save(file));
    assert(state.load(file) && state.get(42, {-2, 3}, 1).rewardClaimed);
    // Migration: a genuine format-1 row remains readable and defaults to an
    // ordinary encounter with no bonus or accepted village destination.
    std::ifstream input(file);
    std::string header, count, row;
    std::getline(input, header); std::getline(input, count); std::getline(input, row);
    input.close();
    std::istringstream fields(row); std::string token, oldRow;
    for (int i = 0; i < 41; ++i) {
        assert(bool(fields >> token));
        oldRow += (i ? " " : "") + token;
    }
    std::ofstream legacy(file, std::ios::trunc);
    legacy << "OverworldEventsState 1\nRecords 1\n" << oldRow << "\nEnd\n";
    legacy.close();
    assert(state.load(file));
    assert(!state.get(42, {-2, 3}, 1).townQuest);
    assert(state.get(42, {-2, 3}, 1).difficultyLevel == 1);
    std::filesystem::remove(file);
    for (unsigned i = 0; i < 33; ++i)
        assert(within(town::nearby(origin, i), origin, 91));
    float hp = 1, damage = 1;
    for (unsigned level = 1; level < 10000000; level = level * 2 + 1) {
        assert(difficulty::health(level) >= hp && difficulty::damage(level) >= damage);
        hp = difficulty::health(level); damage = difficulty::damage(level);
        assert(hp <= 1.4f && damage <= 1.25f);
    }
    assert(difficulty::health(1) == 1.f && difficulty::damage(1) == 1.f);
}
