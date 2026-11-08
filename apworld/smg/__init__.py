"""Super Mario Galaxy, played on the Nebula PC port with the Archipelago mod.

The mod changes nothing by itself. Everything it does is told to it through this world's slot data: which galaxies
are locked and what opens them, which star select entry plays which mission, what an item does.
"""
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

from BaseClasses import CollectionState, Item, ItemClassification, Location, LocationProgressType, Region, Tutorial
from worlds.AutoWorld import WebWorld, World
from worlds.LauncherComponents import Component, Type, components, launch as launch_component

from . import data
from .data import (COMET, COMET_STARTER_STARS, DOME_OPENERS, FINAL_GALAXY, FINAL_STAR, GALAXIES, GALAXY_BY_INTERNAL,
                   GRAND, GREEN_STARS, MAIN, NEED_STARS, PURPLE, PURPLE_COMET_FLAG_STAR, Galaxy, Star)
from .items import (ENEMY_TRAP_TO_ENEMY, ENEMY_TRAPS, FILLER, FILLER_WEIGHTS, GAME, LUIGI_ITEM, POWER_UP_ITEMS,
                    TRAP_OPTIONS, TRAPS, VICTORY, SMGItem, item_name_groups, item_name_to_id, item_table)
from .options import PersistentPowerUps, PurpleCoinComets, SMGOptions, StarShuffle, StartingGalaxy, option_groups

Entry = Tuple[str, int]  # (galaxy, star number): a place in a galaxy's star select, or a star


def launch_client(*args: str) -> None:
    from .client import launch
    launch_component(launch, name="SMGClient", args=args)


components.append(Component("Super Mario Galaxy Client", "SMGClient", func=launch_client,
                            component_type=Type.CLIENT))


class SMGLocation(Location):
    game = GAME


class SMGWeb(WebWorld):
    theme = "partyTime"
    option_groups = option_groups
    tutorials = [Tutorial(
        "Multiworld Setup Guide",
        "Setting up Super Mario Galaxy (Nebula PC port) for Archipelago.",
        "English", "setup_en.md", "setup/en", ["SMG Archipelago"])]


def _location_names() -> Dict[str, int]:
    names: Dict[str, int] = {}
    for g, s in data.all_stars():
        for luigi in (False, True):
            names[data.star_location_name(g.internal, s.number, luigi)] = data.star_location_id(
                g.internal, s.number, luigi)
    return names


CHARACTERS = ("Mario", "Luigi")


class SMGWorld(World):
    """Mario travels the cosmos aboard the Comet Observatory, collecting Power Stars to rescue Princess Peach from
    Bowser. In this randomizer, galaxies and Luigi are opened by items found in other worlds."""
    game = GAME
    web = SMGWeb()
    options_dataclass = SMGOptions
    options: SMGOptions
    topology_present = False

    item_name_to_id = item_name_to_id
    location_name_to_id = _location_names()
    item_name_groups = item_name_groups

    # Set in generate_early.
    open_galaxies: Set[str]
    entry_plays: Dict[Entry, Entry]  # star select entry -> the mission (star) it plays
    keep_owned: Set[Entry]          # stars whose ownership stays where they are played (Grand Stars)
    characters: Tuple[int, ...]

    # ---------------------------------------------------------------- setup
    def generate_early(self) -> None:
        opts = self.options
        if opts.luigi_stars and opts.luigi_unlock != opts.luigi_unlock.option_item:
            # Luigi's stars need a Luigi the logic can reach.
            opts.luigi_unlock.value = opts.luigi_unlock.option_item
        self.characters = (0, 1) if opts.luigi_stars else (0,)

        self.open_galaxies = {"HeavensDoorGalaxy"}
        if opts.starting_galaxy == StartingGalaxy.option_random_terrace:
            choices = [g.internal for g in GALAXIES if g.dome == "Terrace" and not g.needs
                       and not any(s.kind == GRAND for s in g.stars)]
            self.open_galaxies.add(self.random.choice(sorted(choices)))
        else:
            self.open_galaxies.add("EggStarGalaxy")

        self.entry_plays = {(g.internal, s.number): (g.internal, s.number) for g, s in data.all_stars()}
        self.keep_owned = set()
        if opts.star_shuffle != StarShuffle.option_off:
            pools: List[List[Entry]] = []
            if opts.star_shuffle == StarShuffle.option_within_galaxy:
                for g in GALAXIES:
                    pools.append([(g.internal, s.number) for s in g.stars if self._shuffles(g, s)])
            else:
                pools.append([(g.internal, s.number) for g, s in data.all_stars() if self._shuffles(g, s)])
            for pool in pools:
                if len(pool) < 2:
                    continue
                played = list(pool)
                self.random.shuffle(played)
                for entry, star in zip(pool, played):
                    self.entry_plays[entry] = star
            if opts.shuffle_grand_stars:
                self.keep_owned = {(g.internal, s.number) for g, s in data.all_stars() if s.kind == GRAND}

    def _shuffles(self, g: Galaxy, s: Star) -> bool:
        if (g.internal, s.number) in ((FINAL_GALAXY, FINAL_STAR), ("HeavensDoorGalaxy", 1)):
            return False  # the goal, and the prologue's scripted first visit
        if s.kind == MAIN:
            return True
        if s.kind == COMET:
            return bool(self.options.shuffle_comets)
        if s.kind == PURPLE:
            return bool(self.options.shuffle_comets) and self.options.purple_coin_comets == PurpleCoinComets.option_early
        if s.kind == GRAND:
            return bool(self.options.shuffle_grand_stars)
        return False

    def _is_location(self, star: Star) -> bool:
        """Whether a star is a location at all."""
        return not (star.kind == PURPLE and self.options.purple_coin_comets == PurpleCoinComets.option_off)

    # ---------------------------------------------------------------- logic
    def _owned_event(self, c: int, star: Entry) -> str:
        return f"{CHARACTERS[c]} owns {star[0]} {star[1]}"

    def _count_event(self, c: int) -> str:
        return f"{CHARACTERS[c]} Power Star"

    def _owned(self, state: CollectionState, c: int, star: Entry) -> bool:
        return state.has(self._owned_event(c, star), self.player)

    def _stars(self, state: CollectionState, c: int) -> int:
        return state.count(self._count_event(c), self.player)

    def _need(self, state: CollectionState, c: int, need: str) -> bool:
        if need in NEED_STARS:
            return self._owned(state, c, NEED_STARS[need])
        if need.startswith("star:"):
            _, galaxy, number = need.split(":")
            return self._owned(state, c, (galaxy, int(number)))
        if need == "comets":
            return self._stars(state, c) >= COMET_STARTER_STARS
        if need == "purple_comets":
            if self.options.purple_coin_comets == PurpleCoinComets.option_early:
                return self._stars(state, c) >= COMET_STARTER_STARS
            return state.has(VICTORY, self.player)
        if need == "greens":
            return all(self._owned(state, c, star) for star in GREEN_STARS)
        if need == "hungry":
            return True  # star bits are always there to be had
        raise KeyError(need)

    def _dome_open(self, state: CollectionState, c: int, dome: str) -> bool:
        opener = DOME_OPENERS[dome]
        return opener is None or self._owned(state, c, opener)

    def _galaxy_open(self, state: CollectionState, c: int, g: Galaxy) -> bool:
        if c == 1 and not state.has(LUIGI_ITEM, self.player):
            return False
        if not self._dome_open(state, c, g.dome):
            return False
        if not all(self._need(state, c, n) for n in g.needs):
            return False
        if g.internal == FINAL_GALAXY:
            if self._stars(state, c) < self.options.stars_for_bowser:
                return False
        if g.internal in self.open_galaxies:
            return True
        if self.options.galaxy_unlocks:
            return state.has(g.name, self.player)
        return self._stars(state, c) >= g.star_threshold

    def _entry_rule(self, c: int, entry: Entry) -> Callable[[CollectionState], bool]:
        g = GALAXY_BY_INTERNAL[entry[0]]
        s = data.star(*entry)

        def rule(state: CollectionState) -> bool:
            return (self._galaxy_open(state, c, g)
                    and all(self._owned(state, c, (g.internal, k)) for k in s.after)
                    and all(self._need(state, c, n) for n in s.needs))
        return rule

    # ---------------------------------------------------------------- creation
    def create_item(self, name: str) -> Item:
        info = item_table[name]
        classification = info.classification
        if name == LUIGI_ITEM and not self.options.luigi_stars:
            classification = ItemClassification.useful  # nothing in logic waits for him
        return SMGItem(name, classification, info.code, self.player)

    def enabled_traps(self) -> List[str]:
        """Every trap item the options allow."""
        traps = [name for name in sorted(TRAPS) if getattr(self.options, TRAP_OPTIONS[name])]
        if self.options.enemy_traps:
            groups = self.options.enemy_trap_groups.groups()
            traps += [name for name in sorted(ENEMY_TRAPS) if groups[ENEMY_TRAP_TO_ENEMY[name]]["enabled"]]
        return traps

    def get_filler_item_name(self) -> str:
        traps = self.enabled_traps()
        if traps and self.random.randint(1, 100) <= self.options.trap_percentage:
            return self.random.choice(traps)
        names = sorted(FILLER)
        weights = [FILLER_WEIGHTS[n] for n in names]
        power_ups = sorted(self.options.power_up_items.value)
        if power_ups and self.options.power_up_item_weight > 0:
            if self.random.randint(1, sum(weights) + self.options.power_up_item_weight) > sum(weights):
                return self.random.choice(power_ups)
        return self.random.choices(names, weights=weights)[0]

    def create_regions(self) -> None:
        menu = Region("Menu", self.player, self.multiworld)
        observatory = Region("Comet Observatory", self.player, self.multiworld)
        self.multiworld.regions += [menu, observatory]
        menu.connect(observatory)

        for c in self.characters:
            for entry, played in self.entry_plays.items():
                entry_star = data.star(*entry)
                played_star = data.star(*played)
                if not self._is_location(entry_star) or not self._is_location(played_star):
                    continue
                rule = self._entry_rule(c, entry)
                is_goal = c == 0 and played == (FINAL_GALAXY, FINAL_STAR)
                name = data.star_location_name(played[0], played[1], c == 1)
                if is_goal:
                    loc = SMGLocation(self.player, name, None, observatory)
                    loc.place_locked_item(SMGItem(VICTORY, ItemClassification.progression, None, self.player))
                else:
                    loc = SMGLocation(self.player, name, self.location_name_to_id[name], observatory)
                    if entry_star.kind == PURPLE and self.options.purple_coin_comets == PurpleCoinComets.option_vanilla:
                        loc.progress_type = LocationProgressType.EXCLUDED
                loc.access_rule = rule
                observatory.locations.append(loc)

                # What owning the entry's star in the save means to the logic: the entry's star is owned, and one
                # more power star is counted.
                owned = [entry] + ([played] if played in self.keep_owned and played != entry else [])
                for i, star in enumerate(owned):
                    event = SMGLocation(self.player, f"{name} (owned {i})", None, observatory)
                    event.access_rule = rule
                    event_item = SMGItem(self._owned_event(c, star), ItemClassification.progression, None,
                                         self.player)
                    event.place_locked_item(event_item)
                    observatory.locations.append(event)
                counter = SMGLocation(self.player, f"{name} (counted)", None, observatory)
                counter.access_rule = rule
                counter.place_locked_item(SMGItem(self._count_event(c), ItemClassification.progression, None,
                                                  self.player))
                observatory.locations.append(counter)

    def create_items(self) -> None:
        pool: List[Item] = []
        for g in GALAXIES:
            if g.internal not in self.open_galaxies and self.options.galaxy_unlocks:
                pool.append(self.create_item(g.name))
        if self.options.luigi_unlock == self.options.luigi_unlock.option_item:
            pool.append(self.create_item(LUIGI_ITEM))
        slots = len(self.multiworld.get_unfilled_locations(self.player))
        while len(pool) < slots:
            pool.append(self.create_item(self.get_filler_item_name()))
        self.multiworld.itempool += pool

    def set_rules(self) -> None:
        self.multiworld.completion_condition[self.player] = lambda state: state.has(VICTORY, self.player)

    # ---------------------------------------------------------------- what the mod is told
    def _galaxy_rules(self) -> Dict[str, Dict[str, Any]]:
        rules: Dict[str, Dict[str, Any]] = {}
        for g in GALAXIES:
            if g.internal == "HeavensDoorGalaxy":
                continue  # Gateway is the game's own business
            rule: Dict[str, Any] = {"enforce": True}
            if g.internal in self.open_galaxies:
                pass  # always open
            elif self.options.galaxy_unlocks:
                rule["items"] = [g.internal]
            else:
                rule["min_stars"] = g.star_threshold
            if g.internal == FINAL_GALAXY:
                rule["min_stars"] = max(rule.get("min_stars", 0), int(self.options.stars_for_bowser))
            rules[g.internal] = rule
        if self.options.purple_coin_comets == PurpleCoinComets.option_early:
            rules["EventCoin100CometStarter"] = {"flags": ["EventCometStarter"]}
            for galaxy, number in PURPLE_COMET_FLAG_STAR.items():
                rules[f"Coin100Comet{galaxy}"] = {"flags": ["EventCometStarter"], "stars": [[galaxy, number]]}
        return rules

    def fill_slot_data(self) -> Dict[str, Any]:
        luigi_item = self.options.luigi_unlock == self.options.luigi_unlock.option_item
        return {
            "version": 1,
            "galaxies": [g.internal for g in GALAXIES],
            "location_base": data.LOCATION_BASE,
            "luigi_offset": data.LUIGI_LOCATION_OFFSET,
            "stars_per_galaxy": data.STARS_PER_GALAXY_SLOT,
            "rules": self._galaxy_rules(),
            "luigi_rule": {"items": ["@luigi"]} if luigi_item else None,
            "redirects": [[e[0], e[1], p[0], p[1]] for e, p in sorted(self.entry_plays.items()) if e != p],
            "keep": [list(k) for k in sorted(self.keep_owned)],
            "goal": [FINAL_GALAXY, FINAL_STAR],
            "unlock_all_on_goal": bool(self.options.unlock_all_on_goal),
            "luigi_stars": bool(self.options.luigi_stars),
            "item_messages": bool(self.options.item_messages),
            "death_link": bool(self.options.death_link),
            "item_actions": {str(info.code): list(info.action) for info in item_table.values()
                             if info.code is not None},
            "enemy_groups": self._enemy_groups(),
            "power_ups": self._power_up_rules(),
            "skip_cutscenes": {
                "enabled": bool(self.options.skip_cutscenes),
                "never_movies": list(data.ENDING_MOVIES),
                "never_stages": list(data.ENDING_STAGES),
            },
        }

    def _enemy_groups(self) -> Dict[str, List[int]]:
        """The fewest and most of each enemy trap, by the game's object name."""
        groups = self.options.enemy_trap_groups.groups()
        return {data.ENEMY_BY_NAME[name].internal: [g["min"], g["max"]] for name, g in groups.items()}

    def _power_up_rules(self) -> Dict[str, Any]:
        persist = self.options.persistent_power_ups
        kept = [m for m in data.POWER_UPS.values() if m != data.RAINBOW_STAR]
        untimed = bool(self.options.untimed_power_ups)
        return {
            # Which power-ups carry over between levels, and whether only those received as items do.
            "persist": [] if persist == PersistentPowerUps.option_off else sorted(kept),
            "persist_items_only": persist == PersistentPowerUps.option_items_only,
            # Which power-ups don't run out, and the player table's timers to raise for them.
            "untimed": sorted(data.TIMED_POWER_UPS) if untimed else [],
            "constants": {name: data.POWER_UP_TIMER_MAX for name in data.POWER_UP_TIMER_CONSTANTS} if untimed else {},
        }

    def write_spoiler(self, spoiler_handle) -> None:
        moved = [(e, p) for e, p in sorted(self.entry_plays.items()) if e != p]
        if not moved:
            return
        spoiler_handle.write(f"\n\nStar Shuffle ({self.multiworld.player_name[self.player]}):\n")
        for e, p in moved:
            spoiler_handle.write(f"  {data.star_location_name(*e)} -> plays {data.star_location_name(*p)}\n")
