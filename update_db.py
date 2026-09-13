r"""
update_db.py - one command to rebuild the cosmetic catalog after a Dota update.

usage: python update_db.py [--dota <...\dota 2 beta\game\dota>] [--all-global] [--keep-inputs]

1. finds the Dota 2 installation (running dota2.exe, then Steam libraries);
2. extracts scripts/items/items_game.txt, resource/localization/items_english.txt
   and dota_english.txt from pak01 into data/ (no manual copying);
3. runs gen_full_db.py and gen_names.py;
4. runs verify_profile.py so an appearance-profile mismatch is visible now.

Run build.bat afterwards so build\data\skins_full.json is refreshed.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dota_vpk  # noqa: E402

INPUTS = {
    "items_game.txt": "scripts/items/items_game.txt",
    "items_english.txt": "resource/localization/items_english.txt",
    "dota_english.txt": "resource/localization/dota_english.txt",
}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dota", type=Path, default=None, help="Dota's game/dota directory (auto-detected by default)")
    parser.add_argument("--all-global", action="store_true", help="also include server-driven global cosmetics")
    parser.add_argument("--keep-inputs", action="store_true", help="keep the extracted text files in data/")
    args = parser.parse_args(argv)
    game = dota_vpk.game_directory(args.dota)
    if not game:
        print("[!] Dota 2 not found: start Dota once, or pass --dota <...>\\dota 2 beta\\game\\dota")
        return 2
    print(f"[*] Dota 2: {game}")
    archive = dota_vpk.Archive(game)
    data = HERE / "data"
    data.mkdir(exist_ok=True)
    extracted = []
    for name, entry in INPUTS.items():
        if entry not in archive.entries:
            print(f"[!] {entry} is not in pak01_dir.vpk (Dota layout changed?)")
            return 2
        target = data / name
        target.write_bytes(archive.read(entry))
        extracted.append(target)
        print(f"[*] {entry} -> {target} ({target.stat().st_size} bytes)")
    python = sys.executable
    steps = [
        [python, str(HERE / "gen_full_db.py"), "--items-game", str(data / "items_game.txt")] + (["--all-global"] if args.all_global else []),
        [python, str(HERE / "gen_names.py"), "--localization", str(data / "items_english.txt"), str(data / "dota_english.txt")],
    ]
    try:
        for step in steps:
            result = subprocess.run(step)
            if result.returncode:
                print(f"[!] {Path(step[1]).name} failed with exit code {result.returncode}")
                return result.returncode
    finally:
        # These are 60 MB of game files extracted only to feed the generators.
        # Leaving them behind after a failure is how a data folder silently grows.
        if not args.keep_inputs:
            for path in extracted:
                path.unlink(missing_ok=True)
    verify = HERE / "verify_profile.py"
    if verify.exists():
        subprocess.run([python, str(verify), str(game / "bin" / "win64" / "client.dll")])
    print("[OK] data/skins_full.json is up to date. Run build.bat to copy it next to the binaries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
