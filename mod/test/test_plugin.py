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
    ctx.seed_name_for_game = "testseed:1"

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
    await host.run(f"star {to_g} {to_n}")
    await host.run("frames 30")
    want = smg_data.star_location_id(to_g, to_n)
    check(want in checked, f"collecting {to_g} {to_n} checks location {want:#x}")
    out = await host.run("end")
    check(any(f"CALL set_star {entry_g} {entry_n} 1" in l for l in out), "the entry's star becomes owned")
    check(any(f"CALL set_star {to_g} {to_n} 0" in l for l in out), "the played star is given back")

    # Traps and power-ups, sent as they arrive.
    joined = "\n".join(host.lines)
    check("CALL set_player_constant 0 mFireModeTime 65535" in joined
          and "CALL set_player_constant 1 mIceModeTime 65535" in joined, "untimed power-ups raise the timers")
    index = len(ctx.items_received)
    ctx.send_item(index, item("Goomba Trap"), announce=False)
    ctx.send_item(index + 1, item("Fire Flower"), announce=False)
    await host.run("frames 10")
    out = await host.run(f"start {give_token} 1")
    out += await host.run("stage 120")
    low, high = sd["enemy_groups"]["Kuribo"]
    import random
    expected = random.Random(f"testseed:1:{index}").randint(low, high)
    goombas = sum("CALL spawn Kuribo" in l for l in out)
    check(goombas == expected and low <= goombas <= high, f"a Goomba Trap drops {goombas} Goombas ({low}-{high})")
    heights = [float(l.split()[4]) for l in out if "CALL spawn Kuribo" in l]
    check(all(h > 0 for h in heights), "the Goombas drop from above the player")
    out = await host.run("power")
    check("POWER 2" in out, "a Fire Flower item makes Fire Mario")
    out = await host.run("timer")
    out += await host.run("power")
    check("POWER 2" in out, "the Fire Flower does not run out")
    await host.run("end")
    await host.run("reset")
    out = await host.run(f"start {give_token} 2")
    out += await host.run("stage 70")
    out += await host.run("power")
    check("POWER 2" in out, "Fire Mario carries over to the next level")
    out = await host.run("hit")
    out += await host.run("power")
    check("POWER 0" in out, "a hit still takes the power-up away")
    await host.run("end")
    await host.run("reset")
    out = await host.run(f"start {give_token} 3")
    out += await host.run("stage 70")
    out += await host.run("power")
    check("POWER 0" in out, "a power-up lost to a hit stays lost")
    ctx.send_item(index + 2, item("Rainbow Star"), announce=False)
    out = await host.run("stage 70")
    out += await host.run("timer")
    out += await host.run("power")
    check("POWER 0" in out, "the Rainbow Star still runs out")
    await host.run("end")
    ctx.items_received += [item("Goomba Trap"), item("Fire Flower"), item("Rainbow Star")]

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
    await host.quit()
    server.close()

    print()
    print("ALL PASSED" if not failures else f"{len(failures)} FAILED")
    return 1 if failures else 0


sys.exit(asyncio.run(main()))
