"""End-to-end test of the plugin and the client, without the game or a server.

Runs the real SMGContext (apworld/smg/client.py) against the real plugin DLL loaded by mock_host.exe, with slot data
from a generated seed. Usage: python test_plugin.py <Archipelago source dir> <seed output dir>
"""
import asyncio
import glob
import os
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
AP, OUT = sys.argv[1], sys.argv[2]
sys.path.insert(0, AP)
os.chdir(AP)

import Utils  # noqa: E402
from NetUtils import NetworkItem  # noqa: E402
from worlds.smg import client as smg_client  # noqa: E402
from worlds.smg import data as smg_data  # noqa: E402
from worlds.smg.items import item_table  # noqa: E402

PORT = 43779
HOST = os.path.join(HERE, "build", "mock_host.exe")
DLL = os.path.join(HERE, "..", "plugins", "smg_archipelago.dll")
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def load_slot_data():
    path = glob.glob(os.path.join(OUT, "*.archipelago"))[0]
    with open(path, "rb") as f:
        multidata = Utils.restricted_loads(zlib.decompress(f.read()[1:]))
    return multidata["slot_data"][1]


class Host:
    def __init__(self, proc):
        self.proc = proc
        self.lines = []

    @classmethod
    async def start(cls, store):
        proc = await asyncio.create_subprocess_exec(HOST, DLL, store, stdin=asyncio.subprocess.PIPE,
                                                    stdout=asyncio.subprocess.PIPE)
        return cls(proc)

    async def run(self, command):
        """Send one command; return the lines it printed."""
        self.proc.stdin.write((command + "\n").encode())
        await self.proc.stdin.drain()
        out = []
        while True:
            raw = await self.proc.stdout.readline()
            if not raw:
                raise RuntimeError("mock host ended: " + " | ".join(out))
            line = raw.decode().rstrip()
            if line.startswith("DONE"):
                break
            out.append(line)
        self.lines += out
        return out

    async def quit(self):
        await self.run("quit")
        await self.proc.wait()


async def main():
    sd = load_slot_data()
    ctx = smg_client.SMGContext(None, None, PORT)
    ctx.slot = 1
    ctx.seed_name = "testseed"
    ctx.on_package("RoomInfo", {"seed_name": "testseed"})
    ctx.player_names = {1: "Tester", 2: "Friend"}
    checked, statuses = [], []

    async def check_locations(locations):
        checked.extend(locations)

    async def send_msgs(msgs):
        statuses.extend(m for m in msgs if m.get("cmd") == "StatusUpdate")

    ctx.check_locations = check_locations
    ctx.send_msgs = send_msgs
    ctx.missing_locations = set(range(smg_data.LOCATION_BASE, smg_data.LOCATION_BASE + 0x2000))
    ctx.slot_data = sd
    ctx.seed_name_for_game = "testseed:0:1"

    rules = sd["rules"]
    redirects = sd["redirects"]
    open_galaxies = [g for g, r in rules.items() if not r.get("items") and not r.get("min_stars")]
    locked = [g for g, r in rules.items() if r.get("items")]
    redirect = next(r for r in redirects if r[0] in open_galaxies and r[2] != r[0])
    give_token = locked[0]
    still_locked = next(g for g in locked if g != give_token and g != smg_data.FINAL_GALAXY)

    def item(name):
        return NetworkItem(item_table[name].code, 0, 2, 0)
    galaxy_name = smg_data.GALAXY_BY_INTERNAL[give_token].name
    ctx.items_received = [item(galaxy_name), item("1-Up Mushroom"), item("Luigi")]

    server = await asyncio.start_server(ctx.serve_game, "127.0.0.1", PORT)
    store = os.path.join(tempfile.mkdtemp(), "store.txt")
    host = await Host.start(store)
    out = await host.run("frames 120")
    joined = "\n".join(host.lines)
    check("connected to the Archipelago client" in joined, "plugin connects to the client")
    check(f"rules: {len(rules)}" in joined, f"plugin takes the {len(rules)} rules")
    check(ctx.game_version == smg_client.MOD_VERSION and ctx.game_where == "in the menus"
          and ctx.game_status().startswith("connected"),
          f"the client knows the game is connected, and where the player is ({ctx.game_status()})")
    check("Connected to Archipelago as Tester." in joined, "the game is told it's in a multiworld, and as whom")
    ctx.slot_data = None
    ctx.send_status()
    out = await host.run("frames 10")
    check(any("The Archipelago client lost the server." in l for l in out), "the game is told when the server is lost")
    ctx.slot_data = sd
    ctx.send_status()
    await host.run("frames 10")

    out = await host.run(f"canopen {still_locked}")
    check(f"CANOPEN {still_locked} 0" in out, "a locked galaxy can't open, however many stars (no 'new' galaxy in the dome)")
    out = await host.run(f"canopen {give_token}")
    check(f"CANOPEN {give_token} 1" in out, "an unlocked galaxy can")
    label_id = f"ScenarioName_{redirect[0]}{redirect[1]}"
    check(any(f"CALL add_text {label_id} " in l for l in host.lines),
          "the star select names a shuffled entry after the mission it plays")
    out = await host.run(f"start {still_locked} 1")
    out += await host.run("stage 3")
    check(any("CALL back_to_dome" in l for l in out), f"a locked galaxy ({still_locked}) sends the player back")
    given = out

    out = await host.run(f"start {give_token} 1")
    out += await host.run("stage 3")
    check(not any("back_to_dome" in l for l in out), f"an unlocked galaxy ({give_token}) is let in")
    check(any("CALL set_value 5 1" in l for l in given + out), "a 1-Up Mushroom adds a life in a stage")
    await host.run("end")

    entry_g, entry_n, to_g, to_n = redirect
    await host.run(f"flag DUMMY 0")
    out = await host.run(f"start {entry_g} {entry_n}")
    out += await host.run("stage 3")
    check(any(f"CALL change_stage {to_g} {to_n}" in l for l in out),
          f"entry {entry_g} {entry_n} plays {to_g} {to_n}")
    await host.run("end")
    out = await host.run(f"start {to_g} {to_n}")
    out += await host.run("stage 3")
    check(not any("change_stage" in l for l in out), "the redirected stage is not redirected again")
    out = await host.run(f"dome {to_g}")
    entry_dome = (await host.run(f"dome {entry_g}"))
    want_dome = next(l for l in entry_dome if l.startswith("DOME")).split()[-1]
    check(any(l.startswith("DOME") and l.split()[-1] == want_dome for l in out),
          "after the mission you go back to the dome of the galaxy you entered")
    await host.run(f"star {to_g} {to_n}")
    await host.run("frames 30")
    want = smg_data.star_location_id(to_g, to_n)
    check(want in checked, f"collecting {to_g} {to_n} checks location {want:#x}")
    out = await host.run("end")
    check(any(f"CALL set_star {entry_g} {entry_n} 1" in l for l in out), "the entry's star becomes owned")
    check(any(f"CALL set_star {to_g} {to_n} 0" in l for l in out), "the played star is given back")

    # Traps and power-ups, sent as they arrive.
    joined = "\n".join(host.lines)
    check("set_player_constant" not in joined, "with timers on, the game's power-up timers are left alone")
    index = len(ctx.items_received)
    ctx.send_item(index, item("Goomba Trap"), announce=False)
    ctx.send_item(index + 1, item("Fire Flower"), announce=False)
    await host.run("frames 10")
    out = await host.run(f"start {give_token} 1")
    out += await host.run("stage 120")
    low, high = sd["enemy_groups"]["Kuribo"]
    import random
    expected = random.Random(f"testseed:0:1:{index}").randint(low, high)
    goombas = sum("CALL appear Kuribo" in l for l in out)
    check(goombas == expected and low <= goombas <= high, f"a Goomba Trap drops {goombas} Goombas ({low}-{high})")
    spots = [[float(x) for x in l.split()[3:6]] for l in out if "CALL appear Kuribo" in l]
    check(bool(spots) and all(0 < y < 50 for _, y, _ in spots), "the Goombas are put on the ground")
    check(bool(spots) and all(((x - 100) ** 2 + z ** 2) ** 0.5 >= 200 for x, _, z in spots),
          "the Goombas stand round the player")
    check(any("Kuribo after a second: here" in l for l in out), "the first Goomba is still there a second later")
    check("Meramera" not in sd["enemy_pool"], "Li'l Cinder is never readied (the game can't make one outside its levels)")
    pool = (sd["enemy_pool"].get("Kuribo") or [0])[0]
    check("FireBallBeamKameck" in sd["enemy_pool"], "Magikoopas are readied (they warp round the player, with no rail)")
    check(any(f"enemies made ready in {give_token}" in l for l in out) and pool >= high,
          f"the stage readies its trap enemies while it loads ({pool} Goombas)")
    check(not any("object made outside a stage load" in l for l in host.lines),
          "no enemy is made after a stage has loaded (the game can't draw those)")
    # More Goombas than the stage keeps: the ones out longest are reused, so every trap still drops its whole group.
    many = [index + 100 + i for i in range(8)]
    for i in many:
        ctx.send_item(i, item("Goomba Trap"), announce=False)
    flood = await host.run("stage 400")
    want = sum(random.Random(f"testseed:0:1:{i}").randint(low, high) for i in many)
    moved = sum("CALL move Kuribo" in l for l in flood)
    check(want > pool - goombas and moved == want,
          f"traps past the stock reuse the Goombas out longest ({moved} of {want} placed, stock {pool})")
    # Defeated Goombas come back for the next trap, put back in their starting state (not still dying: no star bits).
    await host.run("defeat Kuribo")
    ctx.send_item(index + 200, item("Goomba Trap"), announce=False)
    again = await host.run("stage 200")
    back = sum("MOCK Kuribo back to its start" in l for l in again)
    shown = sum("CALL appear Kuribo" in l for l in again)
    check(back > 0 and back == shown, f"defeated Goombas come back in their starting state ({back} reset, {shown} shown)")
    # Dry Bones can't be put back: once all are defeated, a trap says so and leaves them.
    for i in range(6):  # at least two each: every Dry Bones in the stage has been out
        ctx.send_item(index + 210 + i, item("Dry Bones Trap"), announce=False)
    first = await host.run("stage 300")
    await host.run("defeat Karon")
    ctx.send_item(index + 220, item("Dry Bones Trap"), announce=False)
    second = await host.run("stage 120")
    check(any("CALL appear Karon" in l for l in first) and not any("CALL appear Karon" in l for l in second)
          and any("every Karon in this stage is defeated" in l for l in second),
          "Dry Bones, which can't come back, stay defeated (and the log says why)")
    check(any("Kuribo after a second: here" in l and "not clipped" in l for l in out),
          "the Goombas are taken out of clipping (seen, and able to touch the player)")
    out = await host.run("power")
    check("POWER 2" in out, "a Fire Flower item makes Fire Mario")
    await host.run("end")
    await host.run("reset")
    out = await host.run(f"start {give_token} 2")
    out += await host.run("stage 70")
    out += await host.run("power")
    check("POWER 2" in out, "Fire Mario carries over to the next level")
    out = await host.run("timer")
    out += await host.run("power")
    check("POWER 0" in out, "the Fire Flower runs out on its timer, as in the game")
    await host.run("end")
    await host.run("reset")
    out = await host.run(f"start {give_token} 3")
    out += await host.run("stage 70")
    out += await host.run("power")
    check("POWER 0" in out, "a power-up that ran out stays gone")
    ctx.send_item(index + 2, item("Fire Flower"), announce=False)
    out = await host.run("stage 70")
    out += await host.run("hit")
    out += await host.run("power")
    check("POWER 0" in out, "a hit takes the power-up away")
    ctx.send_item(index + 3, item("Rainbow Star"), announce=False)
    out = await host.run("stage 70")
    out += await host.run("timer")
    out += await host.run("power")
    check("POWER 0" in out, "the Rainbow Star runs out")
    await host.run("end")
    ctx.items_received += [item("Goomba Trap"), item("Fire Flower"), item("Fire Flower"), item("Rainbow Star")]

    # Thwomps, Magikoopas and the Life Mushroom.
    await host.run(f"start {give_token} 1")
    await host.run("stage 10")
    ctx.send_item(index + 300, item("Thwomp Trap"), announce=False)
    thwomp = await host.run("stage 30")
    place = dict(step.split("=", 1) for step in sd["enemy_pool"]["Dossun"][3] if "=" in step)
    height, wait = float(place["f32"].split("=")[1]), int(place["i32"].split("=")[1])
    fields = next((l.split() for l in await host.run("fields Dossun") if l.startswith("FIELDS Dossun")), None)
    check(fields is not None and [round(float(x)) for x in fields[3:6]] == [100, 0, 0]
          and float(fields[7]) == height and int(fields[9]) == wait
          and any("MOCK base matrix Dossun" in l for l in thwomp) and any("CALL appear Dossun" in l for l in thwomp),
          f"a Thwomp is set to come down on the floor under the player, from {height:.0f} up after {wait} frames "
          f"({' '.join(fields[2:]) if fields else 'no fields'})")
    ctx.send_item(index + 301, item("Magikoopa Trap"), announce=False)
    kameck = await host.run("stage 30")
    rail = await host.run("railstart FireBallBeamKameck")
    hide = await host.run("hide FireBallBeamKameck")
    near, far = (float(x) for x in next(s for s in sd["enemy_pool"]["FireBallBeamKameck"][3] if s.startswith("warp=")).split("=")[1:])
    spots = [[float(x) for x in l.split()[2:5]] for l in hide if l.startswith("AT FireBallBeamKameck")]
    check(any("CALL appear FireBallBeamKameck" in l for l in kameck) and not any("MOCK rail start" in l for l in rail),
          "a Magikoopa appears, and stays put when sent to the rail it hasn't got")
    check(bool(spots) and all(near - 1 <= ((x - 100) ** 2 + z ** 2) ** 0.5 <= far + 1 and abs(y - 20) < 1 for x, y, z in spots),
          f"a hiding Magikoopa warps somewhere round the player, {near:.0f}-{far:.0f} away "
          f"({[round(((x - 100) ** 2 + z ** 2) ** 0.5) for x, _, z in spots]})")
    await host.run("busy 1")
    ctx.send_item(index + 302, item("Life Mushroom"), announce=False)
    busy = await host.run("stage 20")
    await host.run("busy 0")
    free = await host.run("stage 20")
    check(not any("MOCK item status 10" in l for l in busy) and sum("MOCK item status 10" in l for l in free) == 1,
          "a Life Mushroom waits until Mario can take it, then is given once, as the game's own pickup")
    await host.run("end")

    # Messages: their text, shown as one of the mod's own messages.
    # (Messages wait their turn, three seconds each: look at whichever were shown.)
    shown = [l for l in host.lines if l.startswith("CALL add_text ArchipelagoMessage ")]
    check(bool(shown) and all(len(l.split(" ", 3)) == 4 and l.split(" ", 3)[3].strip() for l in shown)
          and any("CALL show_message ArchipelagoMessage" in l for l in host.lines),
          f"messages are shown with their text ({shown[0].split(' ', 3)[3] if shown else 'none shown'})")

    # The spin, from the start.
    out = await host.run("swing 0")
    check("MOCK swing 1" in out, "the spin stays allowed when the game would forbid it")

    # The new file's story time.
    out = await host.run("prologue")
    check("PICTUREBOOK closed" in out and "LETTER closed" in out, "the opening story time is skipped")

    # Skipping cutscenes, through Nebula's own skip.
    joined = "\n".join(host.lines)
    check("hooked NebulaCutsceneSkip::update" in joined, "the plugin drives Nebula's cutscene skip")
    await host.run(f"start {give_token} 4")
    await host.run("stage 40")
    await host.run("cutscene 1")
    out = await host.run("stage 40")
    check(any("MOCK skipping" in l for l in out), "a cutscene is skipped")
    check(not any("MOCK prompt shown" in l for l in out), "the skip question is never shown")
    await host.run("cutscene 0")
    out = await host.run("stage 5")
    check(any("MOCK skip done" in l for l in out), "the skip ends with the cutscene")
    await host.run("movie /MovieData/PrologueA.thp")
    out = await host.run("stage 40")
    check(any("MOCK skipping" in l for l in out), "a movie is skipped")
    await host.run("movie off")
    await host.run("stage 5")
    await host.run("movie /MovieData/EndingA.thp")
    out = await host.run("stage 60")
    check(not any("MOCK skipping" in l or "MOCK prompt" in l for l in out), "the ending movie is never skipped")
    await host.run("movie off")
    await host.run("stage 5")
    await host.run("end")
    await host.run(f"start {smg_data.FINAL_GALAXY} 1")
    await host.run("cutscene 1")
    out = await host.run("stage 60")
    check(not any("MOCK skipping" in l for l in out), "nothing is skipped where the ending plays")
    await host.run("cutscene 0")
    await host.run("end")

    # The client going away is said in the game; the client says the game's gone.
    ctx.game_writer.close()
    out = await host.run("frames 30")
    check(any("Lost the Archipelago client" in l for l in out), "the game says when it loses the client")
    check(ctx.game_status().startswith("not connected"), "the client says when the game is gone")

    # Persistence: a new game session without the client still knows the rules.
    await host.quit()
    server.close()
    await server.wait_closed()
    ctx.game_writer = None
    host = await Host.start(store)
    out = await host.run("frames 5")
    check(any("(saved)" in l for l in out), "rules come back from the store with no client")
    out = await host.run(f"start {still_locked} 1")
    out += await host.run("stage 3")
    check(any("CALL back_to_dome" in l for l in out), "a locked galaxy stays locked with no client")
    out = await host.run(f"start {give_token} 1")
    out += await host.run("stage 3")
    check(not any("CALL set_value 5 1" in l for l in out), "a consumable item is not given twice")
    await host.run("end")

    # The goal.
    server = await asyncio.start_server(ctx.serve_game, "127.0.0.1", PORT)
    await host.run("frames 250")
    await host.run(f"start {smg_data.FINAL_GALAXY} 1")
    await host.run(f"star {smg_data.FINAL_GALAXY} 1")
    out = await host.run("stage 20")
    check(any(s.get("status") == 30 for s in statuses), "beating Bowser reports the goal")
    await host.run("end")
    out = await host.run(f"start {still_locked} 1")
    out += await host.run("stage 3")
    check(not any("back_to_dome" in l for l in out), "after the goal every galaxy is open")

    # A client made for another version of the mod (one left running across an update) is told to restart.
    check(not any("restart the client" in l for l in host.lines), "a client of this mod's version is taken without a word")
    ctx.game_send("CLIENT", "0.1.0")
    ctx.game_send("BEGIN")
    ctx.game_send("COMMIT")
    out = await host.run("frames 30")
    check(any("restart the client from the Archipelago Launcher" in l for l in out),
          "a client for another mod version is told to restart")
    ctx.sync_game()
    await host.run("frames 30")

    # A different seed: the client names it by the server's seed name, and the mod starts over.
    ctx.on_package("RoomInfo", {"seed_name": "otherseed"})
    ctx.team = 0
    ctx.on_package("Connected", {"slot_data": sd})
    out = await host.run("frames 30")
    check(ctx.seed_name_for_game == "otherseed:0:1", "the client names the seed by the server's seed name")
    check(any("a new seed: otherseed:0:1" in l for l in out), "a new seed starts the mod's saved state over")
    await host.quit()
    server.close()

    print()
    print("ALL PASSED" if not failures else f"{len(failures)} FAILED")
    return 1 if failures else 0


sys.exit(asyncio.run(main()))
