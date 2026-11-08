# Super Mario Galaxy (Nebula) Setup Guide

## Required software

- Your own copy of Super Mario Galaxy (Wii, North America RMGE01, Europe RMGP01 or Japan RMGJ01).
- [Nebula 2](https://github.com/keepvaibin/nebula), the PC port, built from its `main` branch (Nebula 1.0.2 has no
  mod plugins).
- [Archipelago](https://github.com/ArchipelagoMW/Archipelago/releases) 0.6.4 or newer.
- `smg.apworld` and `Archipelago.luma` from this project.

## Installing

1. Install Nebula 2 and import your disc with `Nebula-Setup.exe`.
2. Double-click `smg.apworld` (or copy it to Archipelago's `custom_worlds` folder).
3. Start Nebula, open Settings (F1) > MODS, and import `Archipelago.luma` (or drop it onto the game window). Nebula
   asks before a mod's code first runs: allow it. The mod plays on its own saves, so your normal saves are untouched.

## Joining a multiworld

1. Generate your YAML from the options page (or Archipelago's Options Creator) and give it to the host.
2. Open the Archipelago Launcher and choose **Super Mario Galaxy Client**. Connect it to the room.
3. Start Nebula. The mod connects to the client by itself (127.0.0.1, port 43770). The client's log says
   "Super Mario Galaxy connected".
4. Start a new file for each new seed.

The client and the mod's port can be changed: the client's `--port` argument and the mod's **Client Port** option
on the Mods page must match.

## Troubleshooting

- `/smg` in the client says whether the game is connected; `/resync` sends the game its rules and items again.
- The mod writes to Nebula's log (lines starting `[archipelago]`).
- If a galaxy you have not unlocked is entered anyway (for example through a save from before), the mod sends you
  back to the observatory.
