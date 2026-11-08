"""Builds dist/Archipelago.luma and dist/smg.apworld. Run mod/build.cmd first."""
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


def pack_apworld() -> str:
    world = os.path.join(ROOT, "apworld", "smg")
    out = os.path.join(DIST, "smg.apworld")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for folder, dirs, files in os.walk(world):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for name in files:
                path = os.path.join(folder, name)
                z.write(path, os.path.join("smg", os.path.relpath(path, world)))
    return out


if __name__ == "__main__":
    os.makedirs(DIST, exist_ok=True)
    print(pack_luma())
    print(pack_apworld())
