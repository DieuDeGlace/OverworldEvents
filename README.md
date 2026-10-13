# OverworldEvents — native encounter test build

Steam Cube World encounters made by DieuDeGlace with the aid of OpenAI ChatGPT/Codex. MIT licensed; SDK attribution is in THIRD_PARTY_NOTICES.md. Targets CubeModLoader API 7.3 and the inspected Steam executable (timestamp `0x5d93ab0d`, image size `0x5b7000`).

This development build compiles and passes four automated tests. Native assets, inventory categories, interaction routes, region scale and AI dispatch were checked against the running Steam executable. Encounter gameplay, cage placement and generated dungeons still require in-game verification; automated checks do not establish those outcomes.

## Travel locations become dungeons

The original regional planner identifies glider, boat, climbing-boots and reins quests (native quest types 6–9). Only those original POI grid coordinates change into a world-seeded native Castle, Ancient Ruins, Crypt or Tower. Other POIs and other special-item quests stay intact. Steam's native generation still creates the terrain, dungeon entities and collision at each fixed site. Revisit/reload chunks after installing; already loaded geometry is not rebuilt in place.

Travel-equipment rewards are suppressed in the native quest query and two verified ground-generation paths, including extra ocean boats. Store purchases, inventory and player item drops remain available. Original planner results are cached by world/seed/region with a bounded cache; a recursion guard prevents replacement choices from moving the original gear sites. Hook signatures are checked before any of the three patches are installed. A conflict or different executable leaves conversion disabled and records the reason in the log. Set `ReplaceTravelLocations=0` before starting the game to retain vanilla travel sites while testing encounters.

## Implemented encounter framework

- Help two adventurers fight four to six enemies drawn from the region's loaded, dry overworld natives. Speak to the adventurer after victory for equipment.
- Defeat a named guardian to free a caged NPC, then speak to the rescued adventurer for equipment.
- An adventurer requests a nearby named boss's defeat. Return and talk after victory for equipment.
- Unlock a chest using its event-specific native key, obtained from a sack or a named guardian. Opening the chest grants equipment directly.

Small encounters sample random sites within Steam's native 8192-block regions, checking actual region ownership at warped boundaries. Tentative sites continue changing as you explore. Once an encounter starts, its location, defeated members, and health snapshots persist for unloading/reloading. After the reward and a default two-hour real-time cooldown, the area must be empty for at least a minute; the next cycle chooses another eligible spot at least 128 blocks from the previous one. Native POI footprints, water, obstructed support and unloaded terrain are excluded. If a reward NPC dies before completion, that event fails and uses the same cooldown/relocation rules.

Default limits are three encounter slots per region and two active events nearby. A battle contains at most six enemies plus two friendly NPCs. Planning runs once per second with at most two terrain searches; there are no render hooks, added GPU passes or background worker threads. Actor construction and atomic state writes occur after terrain locks are released. Actual gameplay overhead has not been measured.

CubeMega owns the existing talk/chest hooks. The updated fork supplies an optional bridge so this DLL does not overwrite those hooks. Native creature construction/appearance and host registration stay in CubeMega. FieldBosses excludes actors owned by an encounter, while named event guardians use its existing purple-light treatment.

Equipment goes through the native drop entry and the installed PyroProgression hook. Numeric gear levels follow the player; a still-resident hostile reward source also allows Pyro's existing chance of enemy-derived higher-level gear. If the source has been unloaded, delivery safely falls back to player-relative levels. Native stars and item rarity remain separate from those numeric levels. Either player-relative or discovery-locked Pyro can be used.

Dialogue currently uses the existing chat text path when the player interacts with an event NPC/object. It does not reproduce Omega's speech-window implementation. No global event-spawn announcement is emitted. Native asset identifiers are resolved by exact runtime dictionary names, and unavailable assets prevent their event type from spawning. Native loot keys use category 22 and the special-items tab; each key is bound to its event and region. Wraiths are excluded from the enemy pool because the common appearance factory does not initialize their teleport home. No proprietary assets or Omega shader/code data are bundled.

A lost/unloaded event key can be recovered from its sack or the defeated guardian's chest before opening it. Only one matching key is granted while already owned; recovery does not grant additional equipment. Battle allies and enemies use native roaming/combat. Quest givers, prisoners and sacks retain the factory's stationary idle behavior; guardians start beyond the native 20-block NPC aggro radius.

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

Placement checks only the positions actually used by each encounter. A sack/chest pair does not need clear, level terrain at unused enemy/companion offsets. Rescue cages still require their complete, flat 7×7 footprint.

Check native enemy movement and damage, friendly combat, cage clearance/removal, NPC reward delivery, sack keys and locked-chest consumption. Leave the area, return, and restart the game to check progress persistence. Dungeon verification requires revisiting an original equipment site after its chunks reload. Single-player/host operation is supported by the implementation; multiplayer synchronization is not yet verified.
