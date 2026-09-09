r"""
gen_names.py - replaces localization tokens in data/skins_full.json with
English names: item names, set (bundle) names and item type names.

usage: python gen_names.py [--db PATH] [--localization FILE ...]

By default it reads items_english.txt and dota_english.txt from data/ or next
to this script (update_db.py extracts both from the game's VPK). Prints how
many tokens stayed unresolved; exit code 1 when more than 30% did.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_FILES = ("items_english.txt", "dota_english.txt")


def load_tokens(paths):
    tokens = {}
    for path in paths:
        text = Path(path).read_text(encoding="utf-8", errors="ignore")
        for key, value in re.findall(r'"([^"\n]+)"\s+"([^"\n]*)"', text):
            tokens.setdefault(key.lower().lstrip("#"), value)
    return tokens


def resolve(text, tokens):
    if not isinstance(text, str) or not text.startswith("#"):
        return text, True
    value = tokens.get(text.lower().lstrip("#"))
    return (value, True) if value is not None else (text, False)


def derive(token):
    """Readable fallback for schema tokens Valve never localized (old default items)."""
    text = token.lstrip("#")
    for prefix in ("DOTA_Item_", "DOTA_Wearable_", "DOTA_", "npc_dota_"):
        if text.startswith(prefix):
            text = text[len(prefix):]
            break
    return text.replace("_", " ").strip() or token


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", type=Path, default=HERE / "data" / "skins_full.json")
    parser.add_argument("--localization", type=Path, nargs="*", default=None)
    args = parser.parse_args(argv)
    files = args.localization or [p for name in DEFAULT_FILES for p in (HERE / "data" / name, HERE / name) if p.exists()]
    if not files:
        print("[!] no localization file found: run update_db.py, or pass --localization")
        return 1
    if not args.db.exists():
        print(f"[!] {args.db} not found: run gen_full_db.py first")
        return 1
    tokens = load_tokens(files)
    print(f"[*] {len(tokens)} localization tokens from {', '.join(str(f) for f in files)}")
    database = json.loads(args.db.read_text(encoding="utf-8"))
    total = resolved = derived = 0
    unresolved_samples = []
    for skin in database.get("skins", []):
        for key in ("name", "type"):
            if key not in skin:
                continue
            total += 1
            skin[key], ok = resolve(skin[key], tokens)
            resolved += ok
            if not ok:
                if len(unresolved_samples) < 5:
                    unresolved_samples.append(skin[key])
                skin[key] = derive(skin[key])
                derived += 1
        for bundle in skin.get("bundles", []):
            total += 1
            bundle["name"], ok = resolve(bundle.get("name", ""), tokens)
            resolved += ok
            if not ok:
                bundle["name"] = derive(bundle["name"])
                derived += 1
    with open(args.db, "w", encoding="utf-8") as handle:
        json.dump(database, handle, indent=1)
    unresolved = total - resolved
    print(f"[OK] names resolved: {resolved}/{total}; {derived} derived from unlocalized tokens")
    if unresolved_samples:
        print("     examples of unlocalized tokens: " + ", ".join(unresolved_samples))
    if total and unresolved > total * 0.30:
        print("[!] too many unresolved tokens: the localization files probably do not match items_game.txt")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
