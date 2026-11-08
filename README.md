# Super Mario Galaxy × Archipelago (Nebula PC port)

An [Archipelago](https://archipelago.gg/) randomizer for Super Mario Galaxy, running on the
[Nebula](https://github.com/keepvaibin/nebula) PC port.

- Every Power Star is a location: collecting it sends an item to someone in the multiworld.
- Galaxies and Luigi are items. Collecting stars never opens a galaxy; someone else has to find its unlock.
- Optional Star Shuffle: swap missions inside each galaxy, or across the whole game.
- Goal: beat Bowser in Bowser's Galaxy Reactor. Optionally, everything unlocks afterwards.

**Every behavior is a YAML option. The mod contains no randomizer data.** It receives its rules from the multiworld's
slot data (through the client), saves them per seed, and with no seed — or with its Archipelago option off — it
passes every call straight through to the game.

## What's here

| Path | What it is |
|---|---|
| `apworld/smg/` | The Archipelago world: items, locations, logic, options, and the **Super Mario Galaxy Client**. |
| `mod/` | The Nebula mod: `luma.json` and the native plugin (`src/smg_archipelago.cpp`). |
| `mod/test/` | A mock Nebula host and an end-to-end test of plugin + client. |
| `package.py` | Builds `dist/Archipelago.luma` and `dist/smg.apworld`. |
| `dist/` | The built files. |

## Setup

See [apworld/smg/docs/setup_en.md](apworld/smg/docs/setup_en.md). In short:

1. Build and install **Nebula 2** from its `main` branch (the released 1.0.2 has no plugins), import your disc.
2. Install `dist/smg.apworld` into Archipelago (double-click it).
3. In Nebula: Settings (F1) > MODS > import `dist/Archipelago.luma`. It uses its own save files.
4. Run **Super Mario Galaxy Client** from the Archipelago Launcher, connect to your room, then start the game.

## Options

| Option | Default | What it does |
|---|---|---|
| `galaxy_unlocks` | on | Galaxies open from items. Off: they open at star-count thresholds the world chooses. |
| `starting_galaxy` | good_egg | Or `random_terrace`. Gateway is always open. |
| `luigi_unlock` | item | Playing as Luigi needs the Luigi item. `vanilla`: after the full ending, as normal. |
| `luigi_stars` | off | Luigi's 120 stars are locations too (forces `luigi_unlock: item`). |
| `star_shuffle` | off | `within_galaxy` or `all_missions`: which mission each star select entry plays. |
| `shuffle_comets` | on | Comet missions join the shuffle. |
| `shuffle_grand_stars` | off | Bowser / Bowser Jr. fights join the shuffle (never the final one or Gateway's). |
| `purple_coin_comets` | early | `early`: they come once Prankster Comets do. `vanilla`: post-game (excluded). `off`: not locations. |
| `stars_for_bowser` | 60 | Stars needed, besides its item, to open Bowser's Galaxy Reactor. |
| `unlock_all_on_goal` | on | Beating Bowser unlocks every galaxy and Luigi. |
| `trap_percentage` | 0 | Share of filler that is traps, picked from the traps switched on below. |
| `damage_trap` | on | Lose one health (never the last). |
| `power_up_loss_trap` | on | Your power-up is taken away. |
| `star_bit_drain_trap` | on | Lose 30 star bits. |
| `enemy_traps` | on | Enemy traps: a group of enemies drops on top of you. |
| `enemy_trap_groups` | see below | Per enemy: whether it can be a trap, and the fewest and most in a group (1–50). |
| `power_up_items` | all | Power-ups that can be filler: Fire Flower, Ice Flower, Bee, Spring, Boo Mushroom, Red Star, Rainbow Star. |
| `power_up_item_weight` | 7 | How often filler is a power-up (the other filler weighs 13 in total). |
| `persistent_power_ups` | all | A power-up stays with you from level to level until lost. `items_only`, or `off`. Never the Rainbow Star. |
| `untimed_power_ups` | on | Fire Flower, Ice Flower and Red Star don't run out. The Rainbow Star still does. |
| `item_messages` | on | In-game messages for items sent and received. |
| `death_link` | off | Standard DeathLink. |

### Enemy trap groups

Each enemy is an item of its own (`Goomba Trap`, `Boo Trap`, ...). When one arrives, a group of between `min` and
`max` of that enemy drops around and above you. Enemies you leave out of the option keep their defaults:

```yaml
Super Mario Galaxy:
  trap_percentage: 20
  enemy_trap_groups:
    Goomba: {enabled: true, min: 3, max: 20}
    Thwomp: {enabled: true, min: 1, max: 1}
    Boo: {enabled: false}
```

| Enemy | Default |  | Enemy | Default |
|---|---|---|---|---|
| Goomba | on, 3–8 | | Li'l Cinder | on, 2–5 |
| Dry Bones | on, 2–5 | | Magikoopa | on, 1–2 |
| Boo | on, 2–6 | | Monty Mole | off, 1–3 |
| Bob-omb | on, 2–6 | | Urchin | off, 2–4 |
| Octoomba | on, 2–5 | | Cluckboom | off, 1–2 |
| Spiky Topman | on, 2–5 | | Thwomp | off, 1–1 |
| Spring Topman | on, 2–5 | | Blooper | off, 2–4 |
| Topmini | on, 3–8 | | Jellyfish | off, 2–4 |

The group size is picked when the item arrives, the same way every time for the same item.

## How it works

```
Archipelago server ⇄ Super Mario Galaxy Client (Python, CommonClient)
                         ⇅ 127.0.0.1:43770, tab-separated lines
                     Nebula ▸ Archipelago.luma ▸ smg_archipelago.dll
```

- **Galaxy locks.** The plugin hooks the game's event-flag checks (`GameEventFlagChecker::isOn`,
  `GameDataHolder::isOnGameEventFlag`, `GameDataFunction::isOnGameEventFlag`, `MR::isOnGameEventFlagGalaxyOpen`).
  For a flag the multiworld has a rule for (a galaxy, a purple comet), it answers from the rule: tokens received,
  other flags, stars owned, star count. If a locked galaxy is entered anyway, the plugin sends the player back to
  the observatory.
- **Luigi.** Hooks on `UserFile::isViewCompleteEnding` and `FileSelector::isUserFileAppearLuigi` make the file select
  offer Luigi only once the Luigi item is in.
- **Star Shuffle.** A same-galaxy swap is made as the star select hands over (`MR::setCurrentScenarioNo`); a
  cross-galaxy one changes stage on the first frame. The star you collect is the played mission's (that is the
  location), and the save records it on the entry you picked, so a galaxy's missions open in order as usual.
- **Stars.** `STAR_GET` events become location checks. The plugin keeps every star it reported and sends them all
  again on each connection, so stars collected while the client is closed still count.
- **Items.** Unlocks are tokens; filler and traps are applied once each (the plugin remembers the last item index).
- **Enemy traps.** The client sends the game's object name and the group size; the plugin spawns them one every few
  frames on a ring above the player, against the local gravity.
- **Power-ups.** A received power-up waits until a stage can take it (the game refuses one whose model the stage
  lacks). With persistence, the plugin gives the carried power-up back after each level loads, and forgets it when
  it's really lost (a hit, water, light, death, a trap). Untimed power-ups: the plugin raises the game's
  `mFireModeTime`/`mIceModeTime` and refuses the timer's "back to normal", so the game restarts the timer instead.

## Building

```bash
mod/build.cmd
```

```bash
python package.py
```

`mod/build.cmd` needs Visual Studio 2022 (or its Build Tools) with the C++ workload.

## Testing

- `apworld`: generated seeds with every option combination tried; Archipelago's `test/general` passes with the
  world installed.
- Plugin + client: `mod/test/test_plugin.py` loads the real DLL in a mock Nebula host and drives it with the real
  client and a generated seed's slot data (galaxy locks, redirects, star checks, save fix-ups, persistence, items,
  goal).

```bash
mod/test/build_mock.cmd
```

```bash
python mod/test/test_plugin.py <Archipelago source folder> <folder with a generated .archipelago>
```

## Known limits / not yet verified in the real game

The plugin has been tested against a mock host, not inside Nebula with the game running. Before trusting a long
seed, check:

- **Hooks.** Nebula's player builds use ThinLTO, which can inline small functions into their callers; a hook on an
  inlined function never fires. The plugin logs `hooked …` / `could not hook …` for each one. The galaxy lock still
  holds without them (the plugin sends you back on entry), but locked galaxies might then *look* open on the map,
  and the Luigi and purple-comet rules need their hooks.
- **Star names and numbers** come from the game's star lists as best known; ids depend only on (galaxy, star number),
  so a wrong name is cosmetic. A few in-game star thresholds (used only with `galaxy_unlocks: off`) are estimates.
- **Hungry Luma and Planet of Trials galaxies** may still want their Luma fed / green stars even with their item;
  the logic already requires that.
- **Cross-galaxy Star Shuffle** loads the entry galaxy briefly before changing to the played one.
- **Enemy traps** use Nebula's `spawn` with no placement data, so some enemies may behave oddly where they don't
  belong (Bloopers and Jellyfish out of water, Monty Moles with no ground to burrow in). Those are off by default.
- **Untimed Red Star** refreshes its timer each minute, which replays the pick-up effect and flying music.
