# gen_full_db.py — dumps EVERYTHING equippable
# input:  scripts/items/items_game.txt  (from game files or github dota2 mirror)
# output: data/skins_full.json
# usage:  copy items_game.txt next to this script, then:  python gen_full_db.py
import re, json, os, sys

SRC = "items_game.txt"
OUT = "data/skins_full.json"

if not os.path.exists(SRC):
    print(f"[!] {SRC} not found. Copy items_game.txt next to this script.")
    sys.exit(0)

print(f"[*] Reading {SRC}...")
src = open(SRC, encoding="utf-8", errors="ignore").read()

def scan_blocks(text):
    """
    Scans text for KeyValues blocks: "key" { ... }
    Correctly matches opening and closing braces at arbitrary nesting depth.
    Yields (key, body).
    """
    pos = 0
    length = len(text)
    pattern = re.compile(r'"([^"]+)"\s*\{')
    while pos < length:
        m = pattern.search(text, pos)
        if not m:
            break
        key = m.group(1)
        start = m.end()
        depth = 1
        idx = start
        while idx < length and depth > 0:
            ch = text[idx]
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
            elif ch == '"':
                idx += 1
                while idx < length and text[idx] != '"':
                    if text[idx] == '\\':
                        idx += 1
                    idx += 1
            idx += 1
        body = text[start:idx - 1]
        yield key, body
        pos = idx

def get_field(body, key):
    m = re.search(r'"%s"\s+"([^"]+)"' % re.escape(key), body, re.IGNORECASE)
    return m.group(1) if m else ""

def get_heroes(body):
    sec = re.search(r'"used_by_heroes"\s*\{(.*?)\}', body, re.S | re.IGNORECASE)
    if not sec:
        return []
    return re.findall(r'"(npc_dota_hero_\w+)"\s+"1"', sec.group(1), re.IGNORECASE)

# Collect playable heroes
HERO_RE = re.compile(r'"(npc_dota_hero_[\w]+)"', re.IGNORECASE)
all_heroes = sorted(set(HERO_RE.findall(src)))
all_heroes = [h.lower() for h in all_heroes if not h.endswith("_base") and "target_dummy" not in h]
print(f"heroes found: {len(all_heroes)}")

# Find bundles
bundles = {}
for top_key, top_body in scan_blocks(src):
    if top_key.lower() == "bundles":
        for bid, b_body in scan_blocks(top_body):
            items_m = re.search(r'"items"\s*\{(.*?)\}', b_body, re.S | re.IGNORECASE)
            items = re.findall(r'"(\d+)"', items_m.group(1)) if items_m else []
            bundles[bid] = {"name": get_field(b_body, "name"), "items": items}

print(f"bundles found: {len(bundles)}")

# Find items
skins = []
seen = set()

for top_key, top_body in scan_blocks(src):
    if top_key.lower() == "items":
        for defid, body in scan_blocks(top_body):
            if not defid.isdigit():
                continue
            if defid in seen:
                continue
            seen.add(defid)

            name = get_field(body, "item_name") or get_field(body, "name")
            prefab = get_field(body, "prefab")
            slot = get_field(body, "item_slot") or get_field(body, "equipment_slot")
            rarity = get_field(body, "item_rarity") or "common"
            heroes = [h.lower() for h in get_heroes(body)]

            if not heroes and not slot:
                continue

            targets = heroes if heroes else ["_global"]
            for h in targets:
                skins.append({
                    "def": int(defid),
                    "name": name,
                    "hero": h,
                    "slot": slot or "misc",
                    "rarity": rarity,
                    "prefab": prefab,
                })

# Attach bundles
for bid, b in bundles.items():
    for item_def in b["items"]:
        for s in skins:
            if str(s["def"]) == item_def:
                s.setdefault("bundles", []).append({"id": int(bid), "name": b["name"]})

os.makedirs("data", exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    json.dump({"heroes": all_heroes, "skins": skins}, f, indent=1)

print(f"[OK] Wrote {OUT}: {len(skins)} wearable entries across {len(all_heroes)} heroes")
