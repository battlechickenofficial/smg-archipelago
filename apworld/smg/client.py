"""The Super Mario Galaxy client: between the Archipelago server and the mod running inside Nebula.

The mod connects to this client over a local TCP socket (127.0.0.1, port 43770 by default) and speaks a line
protocol of tab-separated words. The client turns the world's slot data into the mod's rules, sends received items
to it, and turns the stars the mod reports into location checks.

  client -> mod                                   mod -> client
  SEED <seed>                                     HELLO <version>
  BEGIN                                           STAR <galaxy> <number> <luigi 0/1>
  RULE <flag> <enforce> <min> <items> <flags> <stars>   DEATH
  LUIGIRULE <items>|-                             LOG <text>
  REDIRECT <galaxy> <n> <to galaxy> <to n>
  KEEP <galaxy> <n>
  OPTION <name> <value>
  COMMIT
  TOKEN <token>
  CONST <player table name> <value>
  GIVE <index> <what> <amount>
  TRAP <index> <kind> <amount> [<object>]
  POWERUP <index> <power-up number>
  MSG <text>
  UNLOCKALL
  KILL
"""
import asyncio
import random
from typing import Any, Dict, List, Optional

import Utils
from CommonClient import ClientCommandProcessor, CommonContext, get_base_parser, gui_enabled, logger, server_loop
from NetUtils import ClientStatus, JSONtoTextParser

GAME = "Super Mario Galaxy"
DEFAULT_PORT = 43770
PROTOCOL_VERSION = "1"


def _clean(text: str) -> str:
    return str(text).replace("\t", " ").replace("\n", " ").replace("\r", " ")


class SMGCommandProcessor(ClientCommandProcessor):
    def _cmd_smg(self) -> None:
        """Say whether the game is connected."""
        if isinstance(self.ctx, SMGContext):
            logger.info("Game: " + ("connected" if self.ctx.game_writer else "not connected"))

    def _cmd_resync(self) -> None:
        """Send the game its rules and items again."""
        if isinstance(self.ctx, SMGContext):
            self.ctx.sync_game()
            logger.info("Sent the game its rules and items again.")


class SMGContext(CommonContext):
    command_processor = SMGCommandProcessor
    game = GAME
    items_handling = 0b111

    def __init__(self, server_address: Optional[str], password: Optional[str], port: int) -> None:
        super().__init__(server_address, password)
        self.port = port
        self.game_writer: Optional[asyncio.StreamWriter] = None
        self.slot_data: Optional[Dict[str, Any]] = None
        self.seed_name_for_game: Optional[str] = None
        self.goal_sent = False
        self.text_parser = JSONtoTextParser(self)

    # ------------------------------------------------------------ server side
    async def server_auth(self, password_requested: bool = False) -> None:
        if password_requested and not self.password:
            await super().server_auth(password_requested)
        await self.get_username()
        await self.send_connect()

    def on_package(self, cmd: str, args: Dict[str, Any]) -> None:
        if cmd == "Connected":
            self.slot_data = args.get("slot_data") or {}
            self.seed_name_for_game = f"{self.seed_name}:{self.slot}"
            self.goal_sent = False
            Utils.async_start(self.update_death_link(bool(self.slot_data.get("death_link"))))
            self.sync_game()
        elif cmd == "ReceivedItems":
            start = args["index"]
            if start == 0:
                self.sync_game()
            else:
                for offset, item in enumerate(args["items"]):
                    self.send_item(start + offset, item, announce=True)
        elif cmd == "PrintJSON" and self.slot_data and self.slot_data.get("item_messages"):
            if args.get("type") == "ItemSend":
                item = args["item"]
                receiver = args["receiving"]
                if item.player == self.slot and receiver != self.slot:
                    name = self.item_names.lookup_in_slot(item.item, receiver)
                    self.game_send("MSG", f"Sent {name} to {self.player_names[receiver]}")

    def on_deathlink(self, data: Dict[str, Any]) -> None:
        super().on_deathlink(data)
        self.game_send("KILL")

    async def disconnect(self, allow_autoreconnect: bool = False) -> None:
        self.slot_data = None
        await super().disconnect(allow_autoreconnect)

    # ------------------------------------------------------------ game side
    def game_send(self, *words: Any) -> None:
        if self.game_writer is None:
            return
        try:
            self.game_writer.write(("\t".join(_clean(w) for w in words) + "\n").encode("utf-8"))
        except (ConnectionError, OSError):
            self.game_writer = None

    def sync_game(self) -> None:
        """Everything the mod needs, from the top: its rules, then every item received so far."""
        if self.game_writer is None or not self.slot_data:
            return
        sd = self.slot_data
        self.game_send("SEED", self.seed_name_for_game)
        self.game_send("BEGIN")
        for flag, rule in sd.get("rules", {}).items():
            self.game_send("RULE", flag, int(rule.get("enforce", False)), int(rule.get("min_stars", 0)),
                           ",".join(rule.get("items", [])) or "-",
                           ",".join(rule.get("flags", [])) or "-",
                           ",".join(f"{g}:{n}" for g, n in rule.get("stars", [])) or "-")
        luigi = sd.get("luigi_rule")
        self.game_send("LUIGIRULE", ",".join(luigi["items"]) if luigi else "-")
        for galaxy, number, to_galaxy, to_number in sd.get("redirects", []):
            self.game_send("REDIRECT", galaxy, number, to_galaxy, to_number)
        for galaxy, number in sd.get("keep", []):
            self.game_send("KEEP", galaxy, number)
        self.game_send("OPTION", "messages", int(bool(sd.get("item_messages"))))
        self.game_send("OPTION", "death_link", int(bool(sd.get("death_link"))))
        power = sd.get("power_ups") or {}
        self.game_send("OPTION", "persist", ",".join(str(m) for m in power.get("persist", [])) or "-")
        self.game_send("OPTION", "persist_items_only", int(bool(power.get("persist_items_only"))))
        self.game_send("OPTION", "untimed", ",".join(str(m) for m in power.get("untimed", [])) or "-")
        for name, value in (power.get("constants") or {}).items():
            self.game_send("CONST", name, value)
        skip = sd.get("skip_cutscenes") or {}
        self.game_send("OPTION", "skip_cutscenes", int(bool(skip.get("enabled"))))
        self.game_send("OPTION", "skip_never_movies", ",".join(skip.get("never_movies", [])) or "-")
        self.game_send("OPTION", "skip_never_stages", ",".join(skip.get("never_stages", [])) or "-")
        self.game_send("COMMIT")
        for index, item in enumerate(self.items_received):
            self.send_item(index, item, announce=False)
        if ClientStatus.CLIENT_GOAL == self.client_status_value() and sd.get("unlock_all_on_goal"):
            self.game_send("UNLOCKALL")

    def client_status_value(self) -> int:
        return ClientStatus.CLIENT_GOAL if self.goal_sent or self.finished_game else ClientStatus.CLIENT_UNKNOWN

    def send_item(self, index: int, item: Any, announce: bool) -> None:
        if not self.slot_data:
            return
        action = self.slot_data.get("item_actions", {}).get(str(item.item))
        if not action:
            return
        kind = action[0]
        if kind == "unlock":
            self.game_send("TOKEN", action[1])
        elif kind == "give":
            self.game_send("GIVE", index, action[1], action[2])
        elif kind == "trap" and action[1] == "enemies":
            low, high = (self.slot_data.get("enemy_groups") or {}).get(action[2], (1, 1))
            # The same item always makes the same group, however often it is sent.
            count = random.Random(f"{self.seed_name_for_game}:{index}").randint(low, high)
            self.game_send("TRAP", index, "enemies", count, action[2])
        elif kind == "trap":
            self.game_send("TRAP", index, action[1], action[2])
        elif kind == "power_up":
            self.game_send("POWERUP", index, action[1])
        if announce and self.slot_data.get("item_messages"):
            name = self.item_names.lookup_in_game(item.item)
            sender = self.player_names.get(item.player, "?")
            self.game_send("MSG", f"Got {name}" + ("" if item.player == self.slot else f" from {sender}"))

    def location_for_star(self, galaxy: str, number: int, luigi: bool) -> Optional[int]:
        sd = self.slot_data
        if not sd or galaxy not in sd["galaxies"] or not 0 < number < sd["stars_per_galaxy"]:
            return None
        return (sd["location_base"] + (sd["luigi_offset"] if luigi else 0)
                + sd["galaxies"].index(galaxy) * sd["stars_per_galaxy"] + number)

    async def on_game_line(self, words: List[str]) -> None:
        if not words:
            return
        if words[0] == "HELLO":
            logger.info(f"Super Mario Galaxy connected (mod {words[1] if len(words) > 1 else '?'}).")
            self.sync_game()
        elif words[0] == "STAR" and len(words) >= 4 and self.slot_data:
            galaxy, number, luigi = words[1], int(words[2]), words[3] == "1"
            goal = self.slot_data.get("goal")
            if goal and galaxy == goal[0] and number == goal[1]:
                await self.reach_goal()
            location = self.location_for_star(galaxy, number, luigi)
            if location is not None and location in self.missing_locations:
                await self.check_locations([location])
        elif words[0] == "DEATH" and "DeathLink" in self.tags:
            await self.send_death(f"{self.player_names.get(self.slot, 'Mario')} lost a life.")
        elif words[0] == "LOG" and len(words) > 1:
            logger.info("Game: " + words[1])

    async def reach_goal(self) -> None:
        if not self.goal_sent:
            self.goal_sent = True
            await self.send_msgs([{"cmd": "StatusUpdate", "status": ClientStatus.CLIENT_GOAL}])
            logger.info("Bowser is beaten: goal complete!")
        if self.slot_data and self.slot_data.get("unlock_all_on_goal"):
            self.game_send("UNLOCKALL")

    async def serve_game(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        if self.game_writer is not None:
            try:
                self.game_writer.close()
            except OSError:
                pass
        self.game_writer = writer
        self.game_send("HELLO", PROTOCOL_VERSION)
        try:
            while not reader.at_eof():
                line = await reader.readline()
                if not line:
                    break
                await self.on_game_line(line.decode("utf-8", "replace").rstrip("\r\n").split("\t"))
        except (ConnectionError, OSError):
            pass
        finally:
            if self.game_writer is writer:
                self.game_writer = None
                logger.info("Super Mario Galaxy disconnected.")

    def make_gui(self):
        ui = super().make_gui()
        ui.base_title = "Archipelago Super Mario Galaxy Client"
        return ui


def launch(*args: str) -> None:
    async def main(parsed) -> None:
        ctx = SMGContext(parsed.connect, parsed.password, parsed.port)
        ctx.auth = parsed.name
        ctx.server_task = asyncio.create_task(server_loop(ctx), name="server loop")
        game_server = await asyncio.start_server(ctx.serve_game, "127.0.0.1", parsed.port)
        logger.info(f"Waiting for Super Mario Galaxy on 127.0.0.1:{parsed.port}.")
        if gui_enabled:
            ctx.run_gui()
        ctx.run_cli()
        await ctx.exit_event.wait()
        game_server.close()
        await ctx.shutdown()

    parser = get_base_parser(description="Super Mario Galaxy (Nebula) Archipelago client.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Local port the game's mod connects to.")
    parser.add_argument("--name", default=None, help="Slot name to connect as.")
    parser.add_argument("url", nargs="?", help="Archipelago connection url")
    parsed = parser.parse_args(args)
    if parsed.url:
        from CommonClient import handle_url_arg
        parsed = handle_url_arg(parsed, parser=parser)
    import colorama
    colorama.just_fix_windows_console()
    asyncio.run(main(parsed))
    colorama.deinit()
