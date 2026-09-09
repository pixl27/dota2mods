r"""
gen_full_db.py - builds data/skins_full.json, the cosmetic catalog served to
Dota as the local inventory and browsed by wardrobe.exe.

usage: python gen_full_db.py [--items-game PATH] [--out PATH] [--all-global]

The input is scripts/items/items_game.txt (update_db.py extracts it from the
game's VPK). Requires the `vdf` package (pip install vdf).

Rules, in order:
- prefab inheritance is resolved (items_game "prefabs"), so slot/rarity/type
  come from the prefab chain when an item does not repeat them;
- hero items produce one entry per hero from used_by_heroes;
- client-side global cosmetics (loading screens, HUD skins, music, terrains,
  emblems, cursor packs, announcers, versus screens) produce a "_global" entry;
  --all-global also includes server-driven ones (couriers, wards, creeps...)
  that Dota shows as equipped but a local server never renders;
- containers, keys, tools, stickers, leagues, gems and bundles are not
  wearables and are excluded (sets stay reachable through the "bundles" field).

Exit code 1 when the input is missing or unreadable.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXCLUDED_PREFABS = {
    "bundle", "treasure_chest", "retired_treasure_chest", "key", "socket_gem", "league", "tool",
    "sticker", "sticker_capsule", "dynamic_recipe", "player_card", "emoticon_tool", "showcase_decoration",
}
GLOBAL_CLIENT_PREFABS = {
    "loading_screen", "hud_skin", "music", "terrain", "emblem", "cursor_pack", "announcer", "versus_screen",
}
GLOBAL_SERVER_PREFABS = {
    "courier", "courier_wearable", "ward", "radiantcreeps", "direcreeps", "radianttowers", "diretowers",
    "radiantsiegecreeps", "diresiegecreeps", "roshan", "summons", "teleport_effect", "streak_effect",
    "map_effect", "pennant",
}


def load_items_game(path):
    try:
        import vdf
    except ImportError:
        print("[!] the vdf package is required: pip install vdf")
        sys.exit(1)
    with open(path, encoding="utf-8", errors="ignore") as handle:
        # VDFDict keeps duplicate keys such as repeated asset_modifier blocks.
        root = vdf.load(handle, mapper=vdf.VDFDict)
    return root.get("items_game", root)


def scalar(mapping, key):
    value = mapping.get(key)
    return value if isinstance(value, str) else None


def blocks(mapping, key):
    """All values stored under a (possibly duplicated) key."""
    for name, value in mapping.items():
        plain = name[1] if isinstance(name, tuple) else name
        if plain == key:
            yield value


class Prefabs:
    def __init__(self, prefabs):
        self.prefabs = prefabs

    def resolve(self, item, key):
        own = scalar(item, key)
        if own:
            return own
        seen = set()
        chain = scalar(item, "prefab") or ""
        while chain:
            for name in chain.split():
                if name in seen:
                    continue
                seen.add(name)
                body = self.prefabs.get(name)
                if not isinstance(body, dict):
                    continue
                value = scalar(body, key)
                if value:
                    return value
                parent = scalar(body, "prefab")
                if parent and parent not in seen:
                    chain = parent
                    break
            else:
                chain = ""
        return None


def hero_names(item):
    used = item.get("used_by_heroes", {})
    if isinstance(used, dict):
        return [k.lower() for k, v in used.items() if str(v) == "1" and str(k).lower().startswith("npc_dota_hero_")]
    if isinstance(used, str) and used.lower().startswith("npc_dota_hero_"):
        return [used.lower()]
    return []


def persona_id(item):
    visuals = item.get("visuals")
    if not isinstance(visuals, dict):
        return None
    for modifier in blocks(visuals, "asset_modifier"):
        if isinstance(modifier, dict) and scalar(modifier, "type") == "persona":
            try:
                return int(scalar(modifier, "persona") or 0)
            except ValueError:
                return None
    return None


def style_count(item):
    styles = item.get("visuals", {}).get("styles") if isinstance(item.get("visuals"), dict) else None
    return len(styles) if isinstance(styles, dict) else 0


def build(root, all_global=False):
    prefabs = Prefabs(root.get("prefabs", {}))
    items = root.get("items", {})
    skins, heroes, name_to_def = [], set(), {}
    for defid, body in items.items():
        if not str(defid).isdigit() or not isinstance(body, dict):
            continue
        internal = scalar(body, "name")
        if internal:
            name_to_def[internal.lower()] = int(defid)
        prefab = scalar(body, "prefab") or ""
        prefab_key = prefab.split()[0] if prefab else ""
        if prefab_key in EXCLUDED_PREFABS:
            continue
        targets = hero_names(body)
        for hero in targets:
            if not hero.endswith("_base") and "target_dummy" not in hero:
                heroes.add(hero)
        if not targets:
            if prefab_key in GLOBAL_CLIENT_PREFABS or (all_global and prefab_key in GLOBAL_SERVER_PREFABS):
                targets = ["_global"]
            else:
                continue
        entry = {
            "def": int(defid),
            "name": scalar(body, "item_name") or prefabs.resolve(body, "item_name") or internal or "",
            "slot": prefabs.resolve(body, "item_slot") or scalar(body, "equipment_slot") or ("global" if targets == ["_global"] else "misc"),
            "rarity": prefabs.resolve(body, "item_rarity") or "common",
            "prefab": prefab_key,
        }
        item_type = prefabs.resolve(body, "item_type_name")
        if item_type:
            entry["type"] = item_type
        styles = style_count(body)
        if styles > 1:
            entry["styles"] = styles
        persona = persona_id(body)
        if persona:
            entry["persona"] = persona
        for hero in targets:
            skins.append(dict(entry, hero=hero))

    bundles = {}
    set_index = 100000
    for set_id, set_data in root.get("item_sets", {}).items():
        if not isinstance(set_data, dict):
            continue
        set_name = scalar(set_data, "name") or str(set_id)
        set_items = set_data.get("items", {})
        if isinstance(set_items, dict):
            for item_name in set_items.keys():
                key = item_name[1] if isinstance(item_name, tuple) else item_name
                target = int(key) if str(key).isdigit() else name_to_def.get(str(key).lower())
                if target:
                    bundles.setdefault(target, []).append({"id": set_index, "name": set_name})
        set_index += 1
    for skin in skins:
        if skin["def"] in bundles:
            skin["bundles"] = bundles[skin["def"]]
    return {"heroes": sorted(heroes), "skins": skins}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--items-game", type=Path, default=None, help="items_game.txt (default: data/ or next to this script)")
    parser.add_argument("--out", type=Path, default=HERE / "data" / "skins_full.json")
    parser.add_argument("--all-global", action="store_true", help="also include server-driven global cosmetics")
    args = parser.parse_args(argv)
    source = args.items_game or next((p for p in (HERE / "data" / "items_game.txt", HERE / "items_game.txt") if p.exists()), None)
    if not source or not Path(source).exists():
        print("[!] items_game.txt not found: run update_db.py, or pass --items-game")
        return 1
    print(f"[*] Reading {source}...")
    database = build(load_items_game(source), args.all_global)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(database, handle, indent=1)
    counts = {}
    for skin in database["skins"]:
        counts[skin["prefab"]] = counts.get(skin["prefab"], 0) + 1
    summary = ", ".join(f"{k}={v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])[:8])
    print(f"[OK] Wrote {args.out}: {len(database['skins'])} entries across {len(database['heroes'])} heroes ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
