from dataclasses import dataclass
from typing import Dict, Optional

from BaseClasses import Item, ItemClassification

from .data import ENEMIES, GALAXIES, ITEM_BASE, POWER_UPS

GAME = "Super Mario Galaxy"


class SMGItem(Item):
    game = GAME


@dataclass(frozen=True)
class ItemInfo:
    code: Optional[int]
    classification: ItemClassification
    # What the client tells the mod when the item comes. ("unlock", token), ("give", what, amount), ("trap", kind).
    action: tuple


LUIGI_ITEM = "Luigi"
VICTORY = "Victory"

GALAXY_ITEMS: Dict[str, str] = {g.name: g.internal for g in GALAXIES}

FILLER = {
    "1-Up Mushroom": ("give", "lives", 1),
    "10 Star Bits": ("give", "star_bits", 10),
    "30 Star Bits": ("give", "star_bits", 30),
    "10 Coins": ("give", "coins", 10),
    "Life Refill": ("give", "heal", 3),
}
FILLER_WEIGHTS = {"1-Up Mushroom": 3, "10 Star Bits": 4, "30 Star Bits": 2, "10 Coins": 2, "Life Refill": 2}

# The simple traps, each with the option that turns it on.
TRAPS = {
    "Damage Trap": ("trap", "damage", 1),
    "Power-Up Loss Trap": ("trap", "power_up", 0),
    "Star Bit Drain Trap": ("trap", "star_bits", 30),
}
TRAP_OPTIONS = {
    "Damage Trap": "damage_trap",
    "Power-Up Loss Trap": "power_up_loss_trap",
    "Star Bit Drain Trap": "star_bit_drain_trap",
}

# One trap item per enemy: a group of them lands on the player. How many is chosen when it arrives.
ENEMY_TRAPS = {f"{e.name} Trap": ("trap", "enemies", e.internal) for e in ENEMIES}
ENEMY_TRAP_TO_ENEMY = {f"{e.name} Trap": e.name for e in ENEMIES}

POWER_UP_ITEMS = {name: ("power_up", mode) for name, mode in POWER_UPS.items()}

item_table: Dict[str, ItemInfo] = {}
for i, g in enumerate(GALAXIES):
    item_table[g.name] = ItemInfo(ITEM_BASE + i, ItemClassification.progression, ("unlock", g.internal))
item_table[LUIGI_ITEM] = ItemInfo(ITEM_BASE + 0x100, ItemClassification.progression, ("unlock", "@luigi"))
for i, (name, action) in enumerate(FILLER.items()):
    item_table[name] = ItemInfo(ITEM_BASE + 0x200 + i, ItemClassification.filler, action)
for i, (name, action) in enumerate(TRAPS.items()):
    item_table[name] = ItemInfo(ITEM_BASE + 0x300 + i, ItemClassification.trap, action)
for i, (name, action) in enumerate(ENEMY_TRAPS.items()):
    item_table[name] = ItemInfo(ITEM_BASE + 0x380 + i, ItemClassification.trap, action)
for i, (name, action) in enumerate(POWER_UP_ITEMS.items()):
    item_table[name] = ItemInfo(ITEM_BASE + 0x400 + i, ItemClassification.filler, action)

item_name_to_id: Dict[str, int] = {name: info.code for name, info in item_table.items() if info.code is not None}

item_name_groups = {
    "Galaxies": set(GALAXY_ITEMS),
    "Traps": set(TRAPS) | set(ENEMY_TRAPS),
    "Enemy Traps": set(ENEMY_TRAPS),
    "Power-Ups": set(POWER_UP_ITEMS),
    "Filler": set(FILLER) | set(POWER_UP_ITEMS),
}
