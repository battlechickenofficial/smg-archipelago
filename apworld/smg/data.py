"""Static data for Super Mario Galaxy: galaxies, their stars, and what the game needs to reach them.

Everything here describes the unmodified game. Nothing in this file changes what the game does: the options decide
which of these facts the mod overrides, and the mod is told so through slot data.

Star numbers are the game's own scenario numbers (the order of the in-game star list). The mod reports a collected
star as (internal galaxy name, star number), and the world turns that into a location.
"""
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# Star types.
MAIN = "main"          # a mission picked from the star select
COMET = "comet"        # a Prankster Comet (speed run, daredevil, fast foes, cosmic race)
PURPLE = "purple"      # a purple coin comet (and the purple-coin missions that aren't comets)
HIDDEN = "hidden"      # a hidden star, found inside another mission
GREEN = "green"        # one of the three green stars that open the Planet of Trials
LUIGI = "luigi"        # one of the stars Luigi finds once rescued
GRAND = "grand"        # a Grand Star: the end of a Bowser or Bowser Jr. fight, opens the next dome
RED = "red"            # the red star (Gateway)

# Domes, in the order they open. A dome opens when the Grand Star named for it is owned.
DOMES = ["Gateway", "Terrace", "Fountain", "Kitchen", "Bedroom", "Engine Room", "Garden", "Center"]


@dataclass(frozen=True)
class Star:
    number: int
    name: str
    kind: str
    # Stars of the same galaxy that must already be owned for this star's mission to be offered (by star number).
    after: Tuple[int, ...] = ()
    # Extra needs: "luigi_rescued", "red_star", "grand2", "grand6", "comets", "purple_comets", "greens",
    # "hungry" (a hungry Luma must be fed star bits) and "star:<galaxy>:<number>" (that star owned).
    needs: Tuple[str, ...] = ()


@dataclass(frozen=True)
class Galaxy:
    internal: str
    name: str
    dome: str
    stars: Tuple[Star, ...]
    # Power stars the game asks for before it opens the galaxy. Used when galaxy unlock items are off.
    star_threshold: int = 0
    # Extra needs to reach the galaxy at all (the same words as Star.needs).
    needs: Tuple[str, ...] = ()
    # The galaxy is open from the start: it has no unlock item.
    starts_open: bool = False


def _std(first: str, second: str, third: str, comet: str, purple: str, comet_after: int,
         extra: Tuple[Star, ...] = ()) -> Tuple[Star, ...]:
    """A main galaxy: three missions in order, a comet, a purple coin comet, then its extra stars."""
    return (
        Star(1, first, MAIN),
        Star(2, second, MAIN, after=(1,)),
        Star(3, third, MAIN, after=(2,)),
        Star(4, comet, COMET, after=(comet_after,), needs=("comets",)),
        Star(5, purple, PURPLE, after=(comet_after,), needs=("purple_comets",)),
    ) + extra


def _one(name: str, kind: str = MAIN) -> Tuple[Star, ...]:
    return (Star(1, name, kind),)


GALAXIES: Tuple[Galaxy, ...] = (
    # ---- Gateway (the prologue, outside the domes)
    Galaxy("HeavensDoorGalaxy", "Gateway Galaxy", "Gateway", (
        Star(1, "Grand Star Rescue", GRAND),
        Star(2, "Gateway's Purple Coins", RED, after=(1,), needs=("grand6",)),
    ), starts_open=True),
    Galaxy("TeresaMario2DGalaxy", "Boo's Boneyard Galaxy", "Gateway",
           _one("Racing the Spooky Speedster"), star_threshold=0,
           needs=("grand6", "red_star", "luigi_rescued", "hungry")),

    # ---- Terrace
    Galaxy("EggStarGalaxy", "Good Egg Galaxy", "Terrace", _std(
        "Dino Piranha", "A Snack of Cosmic Proportions", "King Kaliente's Battle Fleet",
        "Dino Piranha Speed Run", "Purple Coin Omelet", 1,
        (Star(6, "Luigi on the Roof", LUIGI, after=(1,), needs=("luigi_rescued",)),)), starts_open=True),
    Galaxy("HoneyBeeKingdomGalaxy", "Honeyhive Galaxy", "Terrace", _std(
        "Bee Mario Takes Flight", "Trouble on the Tower", "Big Bad Bugaboom",
        "Honeyhive Cosmic Mario Race", "The Honeyhive's Purple Coins", 1,
        (Star(6, "Luigi in the Honeyhive Kingdom", LUIGI, after=(1,), needs=("luigi_rescued",)),)), star_threshold=3),
    Galaxy("SurfingLv1Galaxy", "Loopdeeloop Galaxy", "Terrace", _one("Surfing 101"), star_threshold=5),
    Galaxy("FlipPanelExGalaxy", "Flipswitch Galaxy", "Terrace", _one("Painting the Planet Yellow"), star_threshold=7),
    Galaxy("TriLegLv1Galaxy", "Bowser Jr.'s Robot Reactor", "Terrace", _one("Megaleg's Moon", GRAND),
           star_threshold=8),
    Galaxy("BeltConveyerExGalaxy", "Sweet Sweet Galaxy", "Terrace", _one("Rocky Road"), star_threshold=7,
           needs=("hungry",)),

    # ---- Fountain
    Galaxy("StarDustGalaxy", "Space Junk Galaxy", "Fountain", _std(
        "Pull Star Path", "Kamella's Airship Attack", "Tarantox's Tangled Web",
        "Pull Star Path Speed Run", "Space Junk Purple Coin", 2,
        (Star(6, "Yellow Chips and Thin Ice", HIDDEN, after=(1,)),)), star_threshold=9),
    Galaxy("BattleShipGalaxy", "Battlerock Galaxy", "Fountain", _std(
        "Battlerock Barrage", "Breaking into the Battlerock", "Topmaniac's Garbage Dump",
        "Topmaniac's Dardevil Run", "Battlerock's Purple Coins", 3,
        (Star(6, "Battlerock's Garbage Dump", HIDDEN, after=(1,)),
         Star(7, "Luigi under the Saucer", GREEN, after=(1,), needs=("luigi_rescued",)))), star_threshold=12),
    Galaxy("TamakoroExLv1Galaxy", "Rolling Green Galaxy", "Fountain", _one("The Star Ball"), star_threshold=13),
    Galaxy("BreakDownPlanetGalaxy", "Hurry-Scurry Galaxy", "Fountain", _one("Shrinking Satellite"),
           star_threshold=15),
    Galaxy("KoopaBattleVs1Galaxy", "Bowser's Star Reactor", "Fountain", _one("The Fiery Stronghold", GRAND),
           star_threshold=15),
    Galaxy("CocoonExGalaxy", "Sling Pod Galaxy", "Fountain", _one("A Very Sticky Situation"),
           needs=("hungry", "star:StarDustGalaxy:3")),

    # ---- Kitchen
    Galaxy("HeavenlyBeachGalaxy", "Beach Bowl Galaxy", "Kitchen", _std(
        "Sunken Treasure", "Passing the Swim Test", "The Secret Undersea Cavern",
        "Fast Foes on the Cyclone Stone", "Beachcombing for Purple Coins", 3,
        (Star(6, "Wall Jumping Water Fall", HIDDEN, after=(2,)),)), star_threshold=18),
    Galaxy("CubeBubbleExLv1Galaxy", "Bubble Breeze Galaxy", "Kitchen", _one("Through the Poison Swamp"),
           star_threshold=18),
    Galaxy("PhantomGalaxy", "Ghostly Galaxy", "Kitchen", _std(
        "Luigi and the Haunted Mansion", "A Very Spooky Sprint", "Beware of Bouldergeist",
        "Bouldergeist's Daredevil Run", "Purple Coins in the Bone Pen", 3,
        (Star(6, "Matter Splatter Mansion", HIDDEN, after=(1,)),)), star_threshold=20),
    Galaxy("OceanFloaterLandGalaxy", "Buoy Base Galaxy", "Kitchen", (
        Star(1, "The Floating Fortress", MAIN),
        Star(2, "The Secret of Buoy Base", GREEN, after=(1,)),
    ), star_threshold=20),
    Galaxy("KoopaJrShipLv1Galaxy", "Bowser Jr.'s Airship Armada", "Kitchen", _one("Sinking the Airships", GRAND),
           star_threshold=23),
    Galaxy("TearDropGalaxy", "Drip Drop Galaxy", "Kitchen", _one("Drip Drop's Star"),
           needs=("hungry", "star:HeavenlyBeachGalaxy:1")),

    # ---- Bedroom
    Galaxy("CosmosGardenGalaxy", "Gusty Garden Galaxy", "Bedroom", _std(
        "Bunnies in the Wind", "The Dirty Tricks of Major Burrows", "Gusty Garden's Gravity Scramble",
        "Major Burrows's Daredevil Run", "Purple Coin Spacewalk", 3,
        (Star(6, "The Golden Chomp", HIDDEN, after=(1,)),)), star_threshold=24),
    Galaxy("IceVolcanoGalaxy", "Freezeflame Galaxy", "Bedroom", _std(
        "The Frozen Peak of Baron Brrr", "Freezeflame's Blistering Core", "Hot and Cold Collide",
        "Frosty Cosmic Mario Race", "Purple Coins on the Summit", 1,
        (Star(6, "Conquering the Summit", HIDDEN, after=(1,)),)), star_threshold=26),
    Galaxy("SandClockGalaxy", "Dusty Dune Galaxy", "Bedroom", _std(
        "Soaring on the Desert Winds", "Blasting through the Sand", "Sunbaked Sand Castle",
        "Bullet Bill on Your Back", "Purple Coins in the Desert", 3,
        (Star(6, "Treasure of the Pyramid", HIDDEN, after=(1,)),
         Star(7, "Dusty Dune's Green Star", GREEN, after=(1,)))), star_threshold=34),
    Galaxy("HoneyBeeExGalaxy", "Honeyclimb Galaxy", "Bedroom", _one("Scaling the Sticky Wall"), star_threshold=26),
    Galaxy("KoopaBattleVs2Galaxy", "Bowser's Dark Matter Plant", "Bedroom", _one("Darkness on the Horizon", GRAND),
           star_threshold=40),
    Galaxy("FishTunnelGalaxy", "Bigmouth Galaxy", "Bedroom", _one("Gourmet Race"),
           needs=("hungry", "star:SandClockGalaxy:1")),

    # ---- Engine Room
    Galaxy("ReverseKingdomGalaxy", "Gold Leaf Galaxy", "Engine Room", _std(
        "Star Bunnies on the Hunt", "Cataquack to the Skies", "When It Rains, It Pours",
        "Cosmic Mario Forest Race", "Purple Coins in the Woods", 3,
        (Star(6, "The Bell on the Big Tree", HIDDEN, after=(1,)),)), star_threshold=42),
    Galaxy("OceanRingGalaxy", "Sea Slide Galaxy", "Engine Room", _std(
        "Going After Guppy", "Faster Than a Speedrazer", "The Silver Stars of Sea Slide",
        "Underwater Cosmic Mario Race", "Purple Coins by the Seaside", 1,
        (Star(6, "Sea Slide's Hidden Star", HIDDEN, after=(1,)),)), star_threshold=45),
    Galaxy("FactoryGalaxy", "Toy Time Galaxy", "Engine Room", _std(
        "Heavy Metal Mecha-Bowser", "Mario (or Luigi) Meets Mario", "Bouncing Down Cake Lane",
        "Fast Foes of Toy Time", "Luigi's Purple Coins", 3,
        (Star(6, "The Flipswitch Chain", HIDDEN, after=(1,)),)), star_threshold=50),
    Galaxy("SkullSharkGalaxy", "Bonefin Galaxy", "Engine Room", _one("Kingfin's Fearsome Waters"),
           star_threshold=50),
    Galaxy("FloaterOtaKingGalaxy", "Bowser Jr.'s Lava Reactor", "Engine Room",
           _one("King Kaliente's Spicy Return", GRAND), star_threshold=55),
    Galaxy("TransformationExGalaxy", "Sand Spiral Galaxy", "Engine Room", _one("Choosing a Favorite Snack"),
           needs=("hungry", "luigi_rescued", "star:OceanRingGalaxy:2")),

    # ---- Garden
    Galaxy("OceanPhantomCaveGalaxy", "Deep Dark Galaxy", "Garden", _std(
        "The Underground Ghost Ship", "Bubble Blastoff", "Guppy and the Underground Lake",
        "Ghost Ship Daredevil Run", "Plunder the Purple Coins", 2,
        (Star(6, "Boo in a Box", HIDDEN, after=(1,)),)), star_threshold=55),
    Galaxy("CannonFleetGalaxy", "Dreadnought Galaxy", "Garden", _std(
        "Infiltrating the Dreadnought", "Dreadnought's Colossal Cannons", "Revenge of the Topman Tribe",
        "Topman Tribe Speed Run", "Battlestation's Purple Coins", 3,
        (Star(6, "Dreadnought's Garbage Dump", HIDDEN, after=(1,)),)), star_threshold=55),
    Galaxy("DarkRoomGalaxy", "Matter Splatter Galaxy", "Garden", _one("Watch Your Step"), star_threshold=55),
    Galaxy("HellProminenceGalaxy", "Melty Molten Galaxy", "Garden", _std(
        "The Sinking Lava Spire", "Through the Meteor Storm", "Fiery Dino Piranha",
        "Lava Spire Daredevil Run", "Red-Hot Purple Coins", 2,
        (Star(6, "Burning Tide", HIDDEN, after=(1,)),)), star_threshold=60),
    Galaxy("SnowCapsuleGalaxy", "Snow Cap Galaxy", "Garden", _one("Star Bunnies in the Snow"),
           needs=("hungry", "grand6", "star:HellProminenceGalaxy:1")),

    # ---- Planet of Trials (open with the three green stars)
    Galaxy("TamakoroExLv2Galaxy", "Rolling Gizmo Galaxy", "Garden", _one("Gizmos, Gears, and Gadgets"),
           needs=("greens",)),
    Galaxy("CubeBubbleExLv2Galaxy", "Bubble Blast Galaxy", "Garden", _one("The Electric Labyrinth"),
           needs=("greens",)),
    Galaxy("SurfingLv2Galaxy", "Loopdeeswoop Galaxy", "Garden", _one("The Galaxy's Greatest Wave"),
           needs=("greens",)),

    # ---- The center of the observatory
    Galaxy("KoopaBattleVs3Galaxy", "Bowser's Galaxy Reactor", "Center", _one("The Fate of the Universe", GRAND),
           star_threshold=60, needs=("grand6",)),
)

GALAXY_BY_INTERNAL: Dict[str, Galaxy] = {g.internal: g for g in GALAXIES}

# The Grand Star that opens each dome, as (galaxy, star).
DOME_OPENERS: Dict[str, Optional[Tuple[str, int]]] = {
    "Gateway": None,
    "Terrace": ("HeavensDoorGalaxy", 1),
    "Fountain": ("TriLegLv1Galaxy", 1),
    "Kitchen": ("KoopaBattleVs1Galaxy", 1),
    "Bedroom": ("KoopaJrShipLv1Galaxy", 1),
    "Engine Room": ("KoopaBattleVs2Galaxy", 1),
    "Garden": ("FloaterOtaKingGalaxy", 1),
    "Center": ("FloaterOtaKingGalaxy", 1),
}

# Named needs that are one particular star.
NEED_STARS: Dict[str, Tuple[str, int]] = {
    "luigi_rescued": ("PhantomGalaxy", 1),
    "red_star": ("HeavensDoorGalaxy", 2),
    "grand2": ("TriLegLv1Galaxy", 1),
    "grand6": ("FloaterOtaKingGalaxy", 1),
}

GREEN_STARS: Tuple[Tuple[str, int], ...] = tuple(
    (g.internal, s.number) for g in GALAXIES for s in g.stars if s.kind == GREEN)

# The final fight. Its star is the goal and never moves.
FINAL_GALAXY = "KoopaBattleVs3Galaxy"
FINAL_STAR = 1

# Stars the game wants owned before a galaxy's comets come (Prankster Comets start at 13 stars).
COMET_STARTER_STARS = 13

# Game flags the mod is asked to rule over when purple coin comets are brought before the ending.
PURPLE_COMET_STARTER_FLAGS = ("EventCoin100CometStarter", "SpecialStarCoin100CometRelease")

# The game's event flag for each galaxy's purple coin comet, and the star it waits for (the game's own table).
PURPLE_COMET_FLAG_STAR: Dict[str, int] = {
    "EggStarGalaxy": 1, "HoneyBeeKingdomGalaxy": 1, "StarDustGalaxy": 1, "BattleShipGalaxy": 1,
    "HeavenlyBeachGalaxy": 1, "PhantomGalaxy": 2, "CosmosGardenGalaxy": 1, "IceVolcanoGalaxy": 1,
    "SandClockGalaxy": 2, "ReverseKingdomGalaxy": 3, "OceanRingGalaxy": 3, "FactoryGalaxy": 2,
    "OceanPhantomCaveGalaxy": 1, "CannonFleetGalaxy": 2, "HellProminenceGalaxy": 3,
}

# Location and item ids. A location's id is worked out from its galaxy's place in GALAXIES and its star number, so
# the client never needs a table to send one.
LOCATION_BASE = 0x5347_0000  # "SG"
LUIGI_LOCATION_OFFSET = 0x1000
STARS_PER_GALAXY_SLOT = 16
ITEM_BASE = 0x5347_0000


def galaxy_index(internal: str) -> int:
    for i, g in enumerate(GALAXIES):
        if g.internal == internal:
            return i
    raise KeyError(internal)


def star_location_id(internal: str, number: int, luigi: bool = False) -> int:
    return (LOCATION_BASE + (LUIGI_LOCATION_OFFSET if luigi else 0)
            + galaxy_index(internal) * STARS_PER_GALAXY_SLOT + number)


def star_location_name(internal: str, number: int, luigi: bool = False) -> str:
    g = GALAXY_BY_INTERNAL[internal]
    star = next(s for s in g.stars if s.number == number)
    prefix = "Luigi: " if luigi else ""
    return f"{prefix}{g.name} - {star.name}"


def all_stars() -> List[Tuple[Galaxy, Star]]:
    return [(g, s) for g in GALAXIES for s in g.stars]


def star(internal: str, number: int) -> Star:
    return next(s for s in GALAXY_BY_INTERNAL[internal].stars if s.number == number)


# ---- Traps and power-ups. The game's object names and power-up numbers: the mod is sent them, it knows none.

@dataclass(frozen=True)
class Enemy:
    name: str        # what players call it, the key in the enemy_trap_groups option
    internal: str    # the game's object name, as levels place it
    on: bool         # in enemy traps unless the YAML says otherwise
    low: int         # the fewest in a group
    high: int        # the most in a group


ENEMIES: Tuple[Enemy, ...] = (
    Enemy("Goomba", "Kuribo", True, 3, 8),
    Enemy("Dry Bones", "Karon", True, 2, 5),
    Enemy("Boo", "Teresa", True, 2, 6),
    Enemy("Bob-omb", "BombHei", True, 2, 6),
    Enemy("Octoomba", "Takobo", True, 2, 5),
    Enemy("Spiky Topman", "BegomanSpike", True, 2, 5),
    Enemy("Spring Topman", "BegomanSpring", True, 2, 5),
    Enemy("Topmini", "BegomanBaby", True, 3, 8),
    Enemy("Li'l Cinder", "Meramera", True, 2, 5),
    Enemy("Magikoopa", "Kameck", True, 1, 2),
    Enemy("Monty Mole", "Mogu", False, 1, 3),
    Enemy("Urchin", "UnizoLand", False, 2, 4),
    Enemy("Cluckboom", "Poihana", False, 1, 2),
    Enemy("Thwomp", "Dossun", False, 1, 1),
    Enemy("Blooper", "Gesso", False, 2, 4),
    Enemy("Jellyfish", "Jellyfish", False, 2, 4),
)
ENEMY_BY_NAME: Dict[str, Enemy] = {e.name: e for e in ENEMIES}
ENEMY_GROUP_MAX = 50

# The game's power-up numbers (NebulaPowerUp).
POWER_UPS: Dict[str, int] = {
    "Fire Flower": 2,
    "Ice Flower": 3,
    "Bee Mushroom": 4,
    "Spring Mushroom": 5,
    "Boo Mushroom": 6,
    "Red Star": 7,
    "Rainbow Star": 1,
}
RAINBOW_STAR = 1
# The power-ups that run out on a timer (the Rainbow Star does too, and is left to).
TIMED_POWER_UPS: Tuple[int, ...] = (2, 3, 7)
# The player table's timers for those (a u16 of frames): raised to their most when power-ups are untimed.
POWER_UP_TIMER_CONSTANTS: Tuple[str, ...] = ("mFireModeTime", "mIceModeTime")
POWER_UP_TIMER_MAX = 65535
