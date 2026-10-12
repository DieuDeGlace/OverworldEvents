# OverworldEvents — work in progress

Steam Cube World encounters made by DieuDeGlace with the aid of OpenAI ChatGPT/Codex. MIT licensed; SDK attribution is in THIRD_PARTY_NOTICES.md. Targets CubeModLoader API 7.3 and the inspected Steam executable (timestamp `0x5d93ab0d`, image size `0x5b7000`).

This development build compiles and passes the regional rules and persistence tests. Native AI, cage/sack interactions, key identifiers and model placement still require in-game verification. It has not been installed or released as a verified gameplay build. Equipment-location conversion into dungeons is not implemented yet; its native generation route must be identified first.

## Implemented encounter framework

- Help two adventurers fight four to six enemies drawn from the region's loaded, dry overworld natives. Speak to the adventurer after victory for equipment.
- Defeat a named guardian to free a caged NPC, then speak to the rescued adventurer for equipment.
- An adventurer requests a nearby named boss's defeat. Return and talk after victory for equipment.
- Unlock a chest using its event-specific native key, obtained from a sack or a named guardian. Opening the chest grants equipment directly.

Small encounters choose random sites within a region. Once an encounter starts, its location, defeated members, and health snapshots persist for unloading/reloading. After the reward and a default two-hour real-time cooldown, the area must be empty for at least a minute; the next cycle chooses another eligible spot at least 128 blocks from the previous one. Native POI footprints, water, obstructed support and unloaded terrain are excluded.

Default limits are three encounter slots per region and two active events nearby. A battle contains at most six enemies plus two friendly NPCs. Planning runs once per second with at most two terrain searches; there are no render hooks, added GPU passes or background worker threads. Atomic state writes occur on changes after terrain locks are released. Actual gameplay overhead has not been measured.

CubeMega owns the existing talk/chest hooks. The updated fork supplies an optional bridge so this DLL does not overwrite those hooks. Native creature construction/appearance and host registration stay in CubeMega. FieldBosses excludes actors owned by an encounter, while named event guardians use its existing purple-light treatment.

Equipment goes through the native drop entry and the installed PyroProgression hook. Numeric gear levels follow the player; a still-resident hostile reward source also allows Pyro's existing chance of enemy-derived higher-level gear. If the source has been unloaded, delivery safely falls back to player-relative levels. Native stars and item rarity remain separate from those numeric levels. Either player-relative or discovery-locked Pyro can be used.

Dialogue currently uses the existing chat text path when the player interacts with an event NPC/object. It does not reproduce Omega's speech-window implementation. No global event-spawn announcement is emitted. Native asset identifiers are resolved by exact runtime dictionary names, and unavailable assets prevent their event type from spawning. No proprietary assets or Omega shader/code data are bundled.

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

Wait for native verification before treating this as a finished mod. With Steam Cube World closed, the eventual package needs `OverworldEvents.dll`, its configuration, the bridge-enabled CubeMegaMod DLL, and compatible FieldBosses. The current prototype does not replace travel-item locations.

Settings live in `Mods/OverworldEvents.ini`; persistent progress lives in `Mods/OverworldEvents.state`. Keep the state file to preserve completed encounters/cooldowns. Invalid state data stops events rather than overwriting the unreadable file. Existing game saves and the original main branches of the dependency forks are left intact during development.
