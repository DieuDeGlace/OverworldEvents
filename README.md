# OverworldEvents — native encounter test build

Steam Cube World encounters made by DieuDeGlace with the aid of OpenAI ChatGPT/Codex. MIT licensed; SDK attribution is in THIRD_PARTY_NOTICES.md. Targets CubeModLoader API 7.3 and the inspected Steam executable (timestamp `0x5d93ab0d`, image size `0x5b7000`).

This development build compiles and passes eight automated tests, plus two regression tests in its CubeMega bridge. Native assets, inventory categories, interaction routes, region scale and AI dispatch were checked against the inspected Steam executable. The changes below still require live gameplay verification. Automated tests do not establish cage placement, speech rendering, town referrals or generated-dungeon gameplay.

## Travel locations become dungeons

The original regional planner identifies glider, boat, climbing-boots and reins quests (native quest types 6–9). Only those original POI grid coordinates change into a world-seeded native Castle, Ancient Ruins, Crypt or Tower. Other POIs and other special-item quests stay intact. Steam's native generation still creates the terrain, dungeon entities and collision at each fixed site. Revisit/reload chunks after installing; already loaded geometry is not rebuilt in place.

Travel-equipment rewards are suppressed in the native quest query and two verified ground-generation paths, including extra ocean boats. Store purchases, inventory and player item drops remain available. Original planner results are cached by world/seed/region with a bounded cache; a recursion guard prevents replacement choices from moving the original gear sites. Hook signatures are checked before any of the three patches are installed. A conflict or different executable leaves conversion disabled and records the reason in the log. Set `ReplaceTravelLocations=0` before starting the game to retain vanilla travel sites while testing encounters.

## Implemented encounter framework

- Help two adventurers fight four to six enemies drawn from the region's loaded, dry overworld natives. Speak to the adventurer after victory for equipment.
- Defeat a named guardian, then press E at the cage or captive to free the adventurer and receive equipment.
- An adventurer requests a nearby named boss's defeat. Return and talk after victory for equipment.
- Unlock a chest using its event-specific native key, obtained from a sack or a named guardian. Opening the chest grants equipment directly. A key sack lies about 56 blocks from its chest in a world-seeded direction, so the chest is found first and the sack must be searched for.

Ordinary humanoid villagers can offer a rescue, help-in-battle or four-to-six-enemy clearing quest. A town first refers the player to a suitable existing encounter in its region; otherwise it searches up to 32 already-loaded sites outside the village. Native POI, terrain, enemy-pool and encounter-spacing checks still apply. If no eligible site or active slot is available, the villager keeps its normal interaction. Merchants, pets and named NPCs retain their native interactions. Generic villagers in the same town share one referral, allowing hand-in after NPC/chunk reloads. Rewards can be claimed at that town or the encounter's adventurer, with a shared completion flag preventing duplicate delivery. A town clearing quest requires returning to the town; it has no reward NPC at the enemy group. Referrals last for the current world session.

Small encounters sample random sites within Steam's native 8192-block regions, checking actual region ownership at warped boundaries. Tentative sites continue changing as you explore. Once an encounter starts, its site and actors belong to one instance for the current world session. Moving away does not cancel it. If chunks unload, returning recreates only missing roles, retaining the same instance, defeated enemies and health fractions held in memory. Surviving roles are reused to prevent duplicate NPCs, cages and enemies.

A completed or failed encounter stays in the world for the rest of the world session: the opened chest, the freed adventurer and the NPCs remain, and are recreated if their chunks unload and you return. Only unfinished encounters count toward the active-event cap. Leaving the character or quitting ends the session; on a later session, after a default two-hour real-time cooldown and once the area has been empty for at least a minute, the next cycle chooses another eligible spot at least 128 blocks from the previous one. Starting a new world session, including restarting the game, rerolls unfinished encounters and clears their partial progress. Completed and failed encounters retain their recorded cooldowns. Native POI footprints, water, obstructed support and unloaded terrain are excluded. If a battle's reward NPC dies before completion, that event fails and uses the same cooldown/relocation rules.

Encounter sites reserve a 192-block exclusion radius. Natural planning, manual test commands, pending spawns, neighboring-region records and sites still in cooldown share this check, so another encounter cannot be planned on top of a reserved site.

Default limits are three encounter slots per region and two active instances across the current world session. Moving away retains an instance's place in that cap; finish it before starting another when the cap is full. Missing-role restoration fills that instance instead of allocating another encounter. A battle contains at most six enemies plus two friendly NPCs. Planning runs once per second with at most two terrain searches; there are no render hooks, added GPU passes or background worker threads. Actor construction and atomic state writes occur after terrain locks are released. Actual gameplay overhead has not been measured.

CubeMega owns the existing talk/chest hooks. The updated fork supplies an optional bridge so this DLL does not overwrite those hooks. Native creature construction/appearance and host registration stay in CubeMega. FieldBosses excludes actors owned by an encounter, while named event guardians use its existing purple-light treatment. Guardian HP replaces the native named-boss flags' combined 150× multiplier with 3× the ordinary maximum HP for the same race, native stars and numeric level. Pyro's progression and other multiplicative HP adjustments remain applied; the glow is unchanged. This correction applies only to event guardians.

Equipment goes through the native drop entry and the installed PyroProgression hook. Numeric gear levels follow the player; a still-resident hostile reward source also allows Pyro's existing chance of enemy-derived higher-level gear. If the source has been unloaded, delivery safely falls back to player-relative levels. Native stars and item rarity remain separate from those numeric levels. Either player-relative or discovery-locked Pyro can be used.

Rewards use Steam's native chest armor/jewelry and weapon generators. Generation and delivery both check native category, ID, rarity and class/material bounds. Warriors receive native plate and iron weapons; rangers linen/wood gear; mages silk and native wood or metal magic weapons; rogues cotton armor and iron daggers, fists or longswords. CubeMega's Monk can receive native fists, cotton armor or jewelry. Gem materials cannot become armor/weapons, and recipes or spirit-attached items are not fabricated as equipment. Material IDs were checked against the executable's material-name table: iron 1, wood 2, steel 4, gold 11, silver 12, silk 26, linen 27, cotton 28, bronze 29. Gear is generated for the creature that receives it and checked again at delivery. A reward is recorded as claimed only when delivery accepts it; a rejected item undoes the claim so interaction can be retried, and each instance can pay out exactly once. Existing invalid items already in inventory are not rewritten by this change.

Event chests only ever pay out through this mod. The bridge records the exact ID of every actor it spawns, and CubeMega never treats such an actor as an ordinary treasure chest, so an event chest cannot be reopened for repeated free loot. Native world chests are unaffected and keep their normal gold and loot. (An earlier build identified event actors by ID range; native IDs can fall in that range, which stopped native chests paying out and removed native creatures.) Opening or retiring a chest closes both its host and client copies.

Duplicates: actors this mod spawned that no instance owns any more (for example a replaced role) are removed every second; native creatures are never touched. Loading screens and respawns no longer reset tracking, so live actors are never spawned a second time. A rescue captive exists only together with its cage: the cage is created first, and if it unloads or fails, the captive is removed and both are recreated with the captive inside the new cage. This build requires bridge version 2 (CubeMega `test/overworld-events` with these changes); an older bridge leaves events disabled.

Event NPC dialogue uses Steam's native world speech textbox through the CubeMega bridge. Test-command feedback remains in chat; NPC dialogue has no chat fallback. No global event-spawn announcement is emitted. Native asset identifiers are resolved by exact runtime dictionary names, and unavailable assets prevent their event type from spawning. Keys use the native Rusty Key, category 22/item 6, in the special-items inventory tab; each key is bound to its event and region. Wraiths are excluded from the enemy pool because the common appearance factory does not initialize their teleport home. No proprietary assets or Omega shader/code data are bundled.

A lost/unloaded event key can be recovered from its sack or the defeated guardian's chest before opening it. Only one matching key is granted while already owned; recovery does not grant additional equipment. Battle allies and enemies use native roaming/combat. Bounty quest givers and captive adventurers are protected noncombat actors; guardians start beyond the native 20-block NPC aggro radius.

Event cages (native races 194–197), chests (181–184), sacks (242) and captives use native examination behavior 8. Objects carry no armor/weapons and cannot be selected as enemies. Cage placement accounts for the native mesh's center so its base rests on the validated floor; native prisoner binding positions and holds the captive inside it. After the guardian's defeat, E at the cage or captive releases the prisoner and delivers the reward. CubeMega's optional examination adapter routes owned actors through the existing E-key interaction path while retaining the native chest predicate for ordinary objects.

## Build

Windows x64, Visual Studio 2022 C++ Build Tools, Windows SDK, LLVM clang-cl, CMake 3.20+ and Ninja are required. In an x64 Native Tools prompt:

```bat
git clone https://github.com/DieuDeGlace/OverworldEvents.git
cd OverworldEvents
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

The dependency forks' `test/overworld-events` branches contain the optional integration:

- [CubeMegaMod](https://github.com/DieuDeGlace/CubeMegaMod/tree/test/overworld-events): build using its BUILD.md and retain the installed filename `CubeMegaMod-XPC-RU1.dll`.
- [SteamCubeWorldMods](https://github.com/DieuDeGlace/SteamCubeWorldMods/tree/test/overworld-events): build the `FieldBosses` target using its root instructions.

## Development installation

With Steam Cube World closed, install `OverworldEvents.dll` and `config/OverworldEvents.ini` in `Mods`, together with the bridge-enabled CubeMegaMod DLL and compatible FieldBosses. Keep backups of the two existing dependency DLLs. This is a gameplay test build, not a verified release.

Settings live in `Mods/OverworldEvents.ini`; persistent progress lives in `Mods/OverworldEvents.state`. Keep the state file to preserve completed encounters/cooldowns. Invalid state data stops events rather than overwriting the unreadable file. Existing game saves and the original main branches of the dependency forks are left intact during development.

## In-game verification

Type one command in chat while on dry, open overworld terrain:

```text
/events test battle
/events test rescue
/events test bounty
/events test chest-sack
/events test chest-boss
```

These optional commands find an eligible site nearby and print its distance from you. Follow the native chest/NPC/guardian marker on the map; raw engine block coordinates are written only to the log, because they differ from the HUD coordinate format. Markers disappear after reward delivery or event failure. The commands retain POI/terrain checks, active caps and the normal progress/cooldown rules, and never replace a started/completed event. An unused regional slot is required. `/events status` writes dependency/asset/active-state information to `Mods/OverworldEvents.log`.

Only one encounter member anchors an objective-area marker; other NPCs, enemies, sacks and keys remain unmarked. Town enemy groups move the anchor to a surviving enemy when the previous anchor dies. This build uses Steam's existing map icons: a per-objective exclamation-point icon has not been verified and is not substituted into global NPC/shop UI assets.

Placement checks only the positions actually used by each encounter. A sack/chest pair does not need clear, level terrain at unused enemy/companion offsets. Rescue cages still require their complete, flat 7×7 footprint.

Check native enemy movement and damage, guardian HP, friendly combat, cage floor clearance and prisoner binding, speech textboxes, rescue reward delivery, sack keys and locked-chest consumption. Verify reward names/types/materials are natural for the player's class. Leave and return to check that the same unfinished instance restores only missing roles without duplicates. Restart the game to check that unfinished encounters reroll while completed cooldowns remain. In a town, speak to an ordinary villager, follow the single objective marker, finish the objective and return to a villager in the same town; a second hand-in must not give another reward. Dungeon verification requires revisiting an original equipment site after its chunks reload. Single-player/host operation is supported by the implementation; multiplayer synchronization is not yet verified.
