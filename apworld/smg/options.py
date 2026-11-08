from dataclasses import dataclass

from schema import And, Optional, Schema

from Options import (Choice, DeathLink, DefaultOnToggle, OptionDict, OptionGroup, OptionSet, PerGameCommonOptions,
                     Range, Toggle)

from .data import ENEMIES, ENEMY_GROUP_MAX, POWER_UPS


class GalaxyUnlocks(DefaultOnToggle):
    """Galaxies open only when their unlock item is received, never from your own star count.
    Off: galaxies open from your star count, at the thresholds this world chooses (the mod applies them)."""
    display_name = "Galaxy Unlock Items"


class StartingGalaxy(Choice):
    """The galaxy (besides Gateway) that is open from the start when Galaxy Unlock Items is on.
    Random Terrace picks one of the Terrace dome's galaxies that need no other star."""
    display_name = "Starting Galaxy"
    option_good_egg = 0
    option_random_terrace = 1
    default = 0


class LuigiUnlock(Choice):
    """Item: playing as Luigi needs the Luigi item, received from another world.
    Vanilla: Luigi opens as the game opens him, after the full ending."""
    display_name = "Luigi Unlock"
    option_item = 0
    option_vanilla = 1
    default = 0


class LuigiStars(Toggle):
    """Luigi's own 120 stars are locations too. Needs Luigi Unlock set to Item."""
    display_name = "Luigi's Stars"


class StarShuffle(Choice):
    """Which mission a star select entry plays.
    Off: every mission is where it always was.
    Within Galaxy: a galaxy's missions are swapped among that galaxy's entries.
    All Missions: missions of every galaxy are swapped among every galaxy's entries."""
    display_name = "Star Shuffle"
    option_off = 0
    option_within_galaxy = 1
    option_all_missions = 2
    default = 0


class ShuffleComets(DefaultOnToggle):
    """Comet missions take part in Star Shuffle."""
    display_name = "Shuffle Comet Missions"


class ShuffleGrandStars(Toggle):
    """Bowser and Bowser Jr. fights (except the final one) take part in Star Shuffle.
    The domes then open wherever their Grand Star is played."""
    display_name = "Shuffle Grand Star Missions"


class PurpleCoinComets(Choice):
    """Off: purple coin comets are not locations.
    Early: the mod lets purple coin comets come once Prankster Comets come, rather than after the ending.
    Vanilla: they come after the ending as usual, so their stars are only reachable after the goal (excluded)."""
    display_name = "Purple Coin Comets"
    option_off = 0
    option_early = 1
    option_vanilla = 2
    default = 1


class StarsForBowser(Range):
    """Power stars needed, besides the Bowser's Galaxy Reactor item, before the final galaxy opens."""
    display_name = "Stars for Bowser's Galaxy Reactor"
    range_start = 0
    range_end = 100
    default = 60


class UnlockAllOnGoal(DefaultOnToggle):
    """When Bowser is beaten in Bowser's Galaxy Reactor, every galaxy and Luigi are unlocked in your game."""
    display_name = "Unlock Everything on Goal"


class TrapPercentage(Range):
    """Percent of filler items that are traps. Which traps there are is set by the trap options below; with none
    of them on, there are no traps."""
    display_name = "Trap Percentage"
    range_start = 0
    range_end = 100
    default = 0


class DamageTrap(DefaultOnToggle):
    """Damage Trap: lose one health (never the last)."""
    display_name = "Damage Trap"


class PowerUpLossTrap(DefaultOnToggle):
    """Power-Up Loss Trap: your power-up is taken away."""
    display_name = "Power-Up Loss Trap"


class StarBitDrainTrap(DefaultOnToggle):
    """Star Bit Drain Trap: lose 30 star bits."""
    display_name = "Star Bit Drain Trap"


class EnemyTraps(DefaultOnToggle):
    """Enemy traps: a group of enemies drops on top of you. Which enemies, and how many, is Enemy Trap Groups."""
    display_name = "Enemy Traps"


def _enemy_defaults():
    return {e.name: {"enabled": e.on, "min": e.low, "max": e.high} for e in ENEMIES}


_count = And(int, lambda n: 1 <= n <= ENEMY_GROUP_MAX, error=f"a count is from 1 to {ENEMY_GROUP_MAX}")


class EnemyTrapGroups(OptionDict):
    """Each enemy that can be an enemy trap: whether it is one, and the fewest and most in its group. The number in
    each group is picked between min and max when the trap arrives. Enemies left out keep their defaults.

    Enemies: Goomba, Dry Bones, Boo, Bob-omb, Octoomba, Spiky Topman, Spring Topman, Topmini, Li'l Cinder,
    Magikoopa, Monty Mole, Urchin, Cluckboom, Thwomp, Blooper, Jellyfish.

    For example:
      Goomba: {enabled: true, min: 3, max: 20}
      Thwomp: {enabled: false, min: 1, max: 1}"""
    display_name = "Enemy Trap Groups"
    default = _enemy_defaults()
    schema = Schema({
        Optional(e.name): {
            Optional("enabled"): bool,
            Optional("min"): _count,
            Optional("max"): _count,
        } for e in ENEMIES
    })

    def groups(self):
        """Every enemy with its settings, defaults filled in."""
        merged = _enemy_defaults()
        for name, settings in self.value.items():
            merged[name].update(settings)
        for settings in merged.values():
            settings["min"], settings["max"] = sorted((settings["min"], settings["max"]))
        return merged


class PowerUpItems(OptionSet):
    """Power-ups that can be filler items. Leave empty for none."""
    display_name = "Power-Up Items"
    valid_keys = sorted(POWER_UPS)
    default = frozenset(POWER_UPS)


class PowerUpItemWeight(Range):
    """How often a filler item is a power-up, against the other filler (1-Up Mushrooms, star bits, coins and life
    refills have 13 between them)."""
    display_name = "Power-Up Item Weight"
    range_start = 0
    range_end = 50
    default = 7


class PersistentPowerUps(Choice):
    """Whether a power-up stays with you from one level to the next, until it is lost (a hit, water, light...).
    All: any power-up. Items Only: power-ups received as items. Off: as in the game.
    The Rainbow Star never carries over."""
    display_name = "Persistent Power-Ups"
    option_off = 0
    option_items_only = 1
    option_all = 2
    default = 2


class UntimedPowerUps(DefaultOnToggle):
    """The Fire Flower, Ice Flower and Red Star don't run out. The Rainbow Star still does."""
    display_name = "Untimed Power-Ups"


class ItemMessages(DefaultOnToggle):
    """Show a message in the game when an item is sent or received."""
    display_name = "In-Game Item Messages"


@dataclass
class SMGOptions(PerGameCommonOptions):
    galaxy_unlocks: GalaxyUnlocks
    starting_galaxy: StartingGalaxy
    luigi_unlock: LuigiUnlock
    luigi_stars: LuigiStars
    star_shuffle: StarShuffle
    shuffle_comets: ShuffleComets
    shuffle_grand_stars: ShuffleGrandStars
    purple_coin_comets: PurpleCoinComets
    stars_for_bowser: StarsForBowser
    unlock_all_on_goal: UnlockAllOnGoal
    trap_percentage: TrapPercentage
    damage_trap: DamageTrap
    power_up_loss_trap: PowerUpLossTrap
    star_bit_drain_trap: StarBitDrainTrap
    enemy_traps: EnemyTraps
    enemy_trap_groups: EnemyTrapGroups
    power_up_items: PowerUpItems
    power_up_item_weight: PowerUpItemWeight
    persistent_power_ups: PersistentPowerUps
    untimed_power_ups: UntimedPowerUps
    item_messages: ItemMessages
    death_link: DeathLink


option_groups = [
    OptionGroup("Progression", [GalaxyUnlocks, StartingGalaxy, StarsForBowser, UnlockAllOnGoal]),
    OptionGroup("Luigi", [LuigiUnlock, LuigiStars]),
    OptionGroup("Star Shuffle", [StarShuffle, ShuffleComets, ShuffleGrandStars, PurpleCoinComets]),
    OptionGroup("Traps", [TrapPercentage, DamageTrap, PowerUpLossTrap, StarBitDrainTrap, EnemyTraps,
                          EnemyTrapGroups]),
    OptionGroup("Power-Ups", [PowerUpItems, PowerUpItemWeight, PersistentPowerUps, UntimedPowerUps]),
    OptionGroup("Items", [ItemMessages]),
]
