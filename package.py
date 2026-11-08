"""Builds dist/Archipelago.luma and dist/smg.apworld. Run mod/build.cmd first."""
import json
import os
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(ROOT, "dist")


def pack_luma() -> str:
    mod = os.path.join(ROOT, "mod")
    dll = os.path.join(mod, "plugins", "smg_archipelago.dll")
    if not os.path.isfile(dll):
        raise SystemExit("mod/plugins/smg_archipelago.dll is missing: run mod/build.cmd first")
    out = os.path.join(DIST, "Archipelago.luma")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(mod, "luma.json"), "luma.json")  # must be the first entry
        z.write(dll, "plugins/smg_archipelago.dll")
    return out


# What Archipelago's own apworld packager adds to a packaged world's manifest (worlds/Files.py, APWorldContainer):
# the container format it was written in and the oldest one that can read it.
APWORLD_CONTAINER_VERSION = 7


def pack_apworld() -> str:
    world = os.path.join(ROOT, "apworld", "smg")
    out = os.path.join(DIST, "smg.apworld")
    with open(os.path.join(world, "archipelago.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    manifest["version"] = APWORLD_CONTAINER_VERSION
    manifest["compatible_version"] = APWORLD_CONTAINER_VERSION
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for folder, dirs, files in os.walk(world):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for name in files:
                path = os.path.join(folder, name)
                arcname = os.path.join("smg", os.path.relpath(path, world)).replace(os.sep, "/")
                if arcname == "smg/archipelago.json":
                    z.writestr(arcname, json.dumps(manifest, indent=2))
                else:
                    z.write(path, arcname)
    return out


if __name__ == "__main__":
    os.makedirs(DIST, exist_ok=True)
    print(pack_luma())
    print(pack_apworld())
