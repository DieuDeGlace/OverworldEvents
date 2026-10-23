# OverworldEvents — native encounter test build

Steam Cube World encounters made by DieuDeGlace with the aid of OpenAI ChatGPT/Codex. MIT licensed; SDK attribution is in THIRD_PARTY_NOTICES.md. Targets CubeModLoader API 7.3 and the inspected Steam executable (timestamp `0x5d93ab0d`, image size `0x5b7000`).

This build passes eleven event-mod tests and six CubeMega bridge tests. Regression coverage includes accepted-quest persistence/migration, dry destination selection, difficulty bounds, and off-screen D3D11 sprite/transparency/state checks. Phone validation confirms native quest dialogue and the grey full-map destination sprite; a complete combat/hand-in run and PC gameplay remain separate live checks.

## Travel locations become dungeons

The original regional planner identifies glider, boat, climbing-boots and reins quests (native quest types 6–9). Only those original POI grid coordinates change into a world-seeded native Castle, Ancient Ruins, Crypt or Tower. Other POIs and other special-item quests stay intact. Steam's native generation still creates the terrain, dungeon entities and collision at each fixed site. Revisit/reload chunks after installing; already loaded geometry is not rebuilt in place.

Travel-equipment rewards are suppressed in the native quest query and two verified ground-generation paths, including extra ocean boats. Store purchases, inventory and player item drops remain available. Original planner results are cached by world/seed/region with a bounded cache; a recursion guard prevents replacement choices from moving the original gear sites. Hook signatures are checked before any of the three patches are installed. A conflict or different executable leaves conversion disabled and records the reason in the log. Set `ReplaceTravelLocations=0` before starting the game to retain vanilla travel sites while testing encounters.

## Implemented encounter framework

- Help two adventurers fight four to six enemies drawn from the region's loaded, dry overworld natives. Speak to the adventurer after victory for equipment.
- Defeat a named guardian, then press E at the cage or captive to free the adventurer and receive equipment.
- An adventurer requests a nearby named boss's defeat. Return and talk after victory for equipment.
- Unlock a chest using its event-specific native key, obtained from a sack or a named guardian. Opening the chest grants equipment directly. A key sack lies about 56 blocks from its chest in a world-seeded direction, so the chest is found first and the sack must be searched for.

## Landmark encounters

Every natural native location that holds no utility item hosts an encounter of its own, placed at the location itself:

- **Hill, Canyon, Crater, Rocks, Rock, Mountain, Volcano, Tree (mana tree), Forest, Dark/Enchanted Forest, Grove, Glade, Plain, Island, Beach, Arena:** a named boss holding the center, with 3-5 of the region's own enemies around it.
- **Camp, Bandit Camp, Old Hut:** a named leader with 4-5 followers.
- **Altar, Sanctuary, Menhir, Circle of Power, Graveyard:** a named guardian with 2-3 minions.

A location where the native planner placed a utility-item quest (native quests 6-13: glider, boat, reins, climbing boots and the other category-24 specials such as the harp and whistle) keeps its item and gets no encounter. The check reads the same cached native planner output as the travel-site conversion. Enemies are drawn from the region's loaded, dry overworld natives and spawn in the encounter's region, so their numeric level is that region's. When a large centerpiece occupies the exact center (a mana tree's trunk, for instance), the group is placed up to about 34 blocks beside it.

When the whole group has fallen, an unlocked reward chest appears at the center; it pays out once through the same class-checked delivery as other events. The leader uses the guardian HP rule and native boss star bonus. Lakes are left to CubeMegaMod's lake encounters and mana pumps to their native Iron Legion population; villages, houses, farms, inns and dungeons are unchanged. Native location types were checked against the executable's location-name table.

Locations are found with native location queries on a 128-block lattice around the player (they read the world plan and generate no terrain); an encounter starts when you come within about 160 blocks. Each landmark has its own record in reserved per-region slots 8-31 (random encounters use the lower slots), so a finished landmark stays done for the session and returns, rerolled, after its cooldown. Landmark encounters have their own active cap, `LandmarkEventCap` (default 2, 0 disables them), separate from `ActiveEventCap`. The map shows the mission-tag icon on the group, then on the reward chest. `/events landmarks` writes the qualifying locations near you to the log, plus the ones skipped because they hold a utility item.

Village NPCs offer battle, rescue and enemy-group quests. A town can refer the player to a suitable existing encounter, or reserve a destination before its chunks load. The marker and interaction share the same offer predicate. Procedural height samples reject water and steep footprints; native location, region and spacing checks still apply. Villagers in a town share a five-second search cache. On approach, terrain validation checks at most four nearby sites per accepted quest each second, and actors are constructed only after terrain locks are released. An accepted objective reserves an active slot immediately, survives world reloads with its progress, and can be handed in at town even while its actors are unloaded. Merchants, pets and named native NPCs retain their own interactions.

Small encounters sample random sites within Steam's native 8192-block regions, checking actual region ownership at warped boundaries. Tentative sites continue changing as you explore. Once an encounter starts, its site and actors belong to one instance for the current world session. Moving away does not cancel it. If chunks unload, returning recreates only missing roles, retaining the same instance, defeated enemies and health fractions held in memory. Surviving roles are reused to prevent duplicate NPCs, cages and enemies.

A completed or failed encounter stays in the world for the rest of the world session: the opened chest, the freed adventurer and the NPCs remain, and are recreated if their chunks unload and you return. Only unfinished encounters count toward the active-event cap. Leaving the character or quitting ends the session; on a later session, after a default two-hour real-time cooldown and once the area has been empty for at least a minute, the next cycle chooses another eligible spot at least 128 blocks from the previous one. Starting a new world session rerolls unfinished random encounters; accepted village quests retain their destinations and progress. Completed and failed encounters retain their recorded cooldowns. Native POI footprints, water, obstructed support and unloaded terrain are excluded. If a battle's reward NPC dies before completion, that event fails and uses the same cooldown/relocation rules.

Encounter sites reserve a 192-block exclusion radius. Natural planning, manual test commands, pending spawns, neighboring-region records and sites still in cooldown share this check, so another encounter cannot be planned on top of a reserved site.

Default limits are three encounter slots per region and two active instances across the current world session. Moving away retains an instance's place in that cap; finish it before starting another when the cap is full. Missing-role restoration fills that instance instead of allocating another encounter. A battle contains at most six enemies plus two friendly NPCs. Random planning runs once per second with at most two terrain searches. Accepted village objectives have a separate bounded search. Quest sprites use one small D3D11 batch after scene antialiasing, with full pipeline restoration and point sampling; there are no background worker threads. Actor construction and atomic state writes occur after terrain locks are released. Actual gameplay overhead has not been measured.

CubeMega owns the existing talk/chest hooks. The updated fork supplies an optional bridge so this DLL does not overwrite those hooks. Native creature construction/appearance and host registration stay in CubeMega. FieldBosses excludes actors owned by an encounter, while named event guardians use its existing purple-light treatment. Guardian HP replaces the native named-boss flags' combined 150× multiplier with 3× the ordinary maximum HP for the same race, native stars and numeric level. Pyro's progression and other multiplicative HP adjustments remain applied; the glow is unchanged. This correction applies only to event guardians.

Equipment goes through the native drop entry and the installed PyroProgression hook. Numeric gear levels follow the player; a still-resident hostile reward source also allows Pyro's existing chance of enemy-derived higher-level gear. If the source has been unloaded, delivery safely falls back to player-relative levels. Native stars and item rarity remain separate from those numeric levels. Either player-relative or discovery-locked Pyro can be used.

Rewards use Steam's native chest armor/jewelry and weapon generators. Generation and delivery both check native category, ID, rarity and class/material bounds. Warriors receive native plate and iron weapons; rangers linen/wood gear; mages silk and native wood or metal magic weapons; rogues cotton armor and iron daggers, fists or longswords. CubeMega's Monk can receive native fists, cotton armor or jewelry. Gem materials cannot become armor/weapons, and recipes or spirit-attached items are not fabricated as equipment. Material IDs were checked against the executable's material-name table: iron 1, wood 2, steel 4, gold 11, silver 12, silk 26, linen 27, cotton 28, bronze 29. Gear is generated for the creature that receives it and checked again at delivery. A reward is recorded as claimed only when delivery accepts it; a rejected item undoes the claim so interaction can be retried, and each instance can pay out exactly once. Existing invalid items already in inventory are not rewritten by this change.

Event chests only ever pay out through this mod. The bridge records the exact ID of every actor it spawns, and CubeMega never treats such an actor as an ordinary treasure chest, so an event chest cannot be reopened for repeated free loot. Native world chests are unaffected and keep their normal gold and loot. (An earlier build identified event actors by ID range; native IDs can fall in that range, which stopped native chests paying out and removed native creatures.) Opening or retiring a chest closes both its host and client copies.

Duplicates: actors this mod spawned that no instance owns any more (for example a replaced role) are removed every second; native creatures are never touched. Loading screens and respawns no longer reset tracking, so live actors are never spawned a second time. A rescue captive exists only together with its cage: the cage is created first, and if it unloads or fails, the captive is removed and both are recreated with the captive inside the new cage. This build requires bridge version 2 (CubeMega `test/overworld-events` with these changes); an older bridge leaves events disabled.

Event NPC dialogue uses Steam's native world speech textbox through the CubeMega bridge. If another UI hook prevents a native speaker widget, the bridge keeps the quest readable in chat. Test-command feedback remains in chat. No global event-spawn announcement is emitted. Native asset identifiers are resolved by exact runtime dictionary names, and unavailable assets prevent their event type from spawning. Keys use the native Rusty Key, category 22/item 6, in the special-items inventory tab; each key is bound to its event and region. Wraiths are excluded from the enemy pool because the common appearance factory does not initialize their teleport home. No proprietary assets or Omega shader/code data are bundled.

A lost/unloaded event key can be recovered from its sack or the defeated guardian's chest before opening it. Only one matching key is granted while already owned; recovery does not grant additional equipment. Battle allies and enemies use native roaming/combat. Bounty quest givers and captive adventurers are protected noncombat actors; guardians start beyond the native 20-block NPC aggro radius.

Event cages (native races 194–197), chests (181–184), sacks (242) and captives use native examination behavior 8. Placed objects (cages, sacks, chests) are held at their spawn position every frame and their velocity cleared, so walking or fighting into them cannot push them; an unfreed captive stays bound inside its cage. Objects keep the native appearance generated for their race and are never resized. With PyroProgression installed they show no level on their nameplate.

Quest sprites use the supplied `assets/quest-icons.png`: an orange exclamation mark above available village quest givers, an orange question mark on the full world map for random encounters, and a grey question mark for an accepted village objective. Completing that objective moves its grey pin back to town; a ready hand-in uses the orange question mark overhead. Updated PyroProgression supplies the native nameplate coordinates after the game's visibility/line-of-sight tests, without putting punctuation in names. Native map icons remain the minimap and unsupported-GPU fallback. The PNG is embedded losslessly in generated `src/QuestAtlas.h`; `tools/generate_quest_atlas.py` regenerates it (Pillow needed only for that optional tool). Green is color-keyed at rendering time, preserving the original outlines.

Encounter difficulty retains native stars and Pyro's existing region/discovery rules. A snapshot of the higher of player and sampled regional enemy levels adds `2% * sqrt(level - 1)` health and `1.25% * sqrt(level - 1)` attack/spell power, capped at +40% and +25%. Level-one encounters get no added premium. The optional `PyroProgressionCreatureLevel` export supplies the same numeric levels used by enemy nameplates. Only registered encounter enemies receive these modifiers; ordinary enemies, friendly actors and the player are unchanged.

State format 2 reads existing format-1 files and writes village ownership, pending-site state and difficulty alongside encounter progress. Back up `Mods/OverworldEvents.state` before downgrading to an older DLL; older builds cannot read format 2.

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

World-map objective icons: each unfinished encounter is marked with the native mission tag, tinted yellow, at its objective (the chest, the adventurer or a surviving enemy of a group). The icon is added to the map's own icon list just before the map sorts and draws it, so it tilts, scales and sorts with the map like native icons; it stays at the last known position while the objective is unloaded, and disappears once the encounter is finished. Event actors are no longer drawn on the map as their own models. Nothing persistent is changed: the icon list is rebuilt by the game every frame. The hook at map routine RVA 0x285a35 (where the icon loop and its skip branch meet) is installed only when its 14 original bytes and the native push function match; each frame it uses an existing native icon as the template and requires that icon's sprite to be a known GUI sprite, otherwise it adds nothing. If the hook cannot be installed, the previous behavior (the objective actor shown on the map) is kept. `MapMarkerSprite` picks the icon: `missiontag` (default), `destination`, `star`, `cross`, `key` or `treasure`. The first map frame, and any later change, writes a `Map icons:` line to the log stating whether markers were added or why not. Set `MapMarkers=0` to disable it. The sprites' appearance on the map has not yet been seen in game.

Placement checks only the positions actually used by each encounter. A sack/chest pair does not need clear, level terrain at unused enemy/companion offsets. Rescue cages still require their complete, flat 7×7 footprint.

Check native enemy movement and damage, guardian HP, friendly combat, cage floor clearance and prisoner binding, speech textboxes, rescue reward delivery, sack keys and locked-chest consumption. Verify reward names/types/materials are natural for the player's class. Leave and return to check that the same unfinished instance restores only missing roles without duplicates. Restart the game to check that unfinished encounters reroll while completed cooldowns remain. In a town, speak to an ordinary villager, follow the single objective marker, finish the objective and return to a villager in the same town; a second hand-in must not give another reward. Dungeon verification requires revisiting an original equipment site after its chunks reload. Single-player/host operation is supported by the implementation; multiplayer synchronization is not yet verified.
