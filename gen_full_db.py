# gen_full_db.py — dumps EVERYTHING equippable
# input:  scripts/items/items_game.txt  (from game files or github dota2 mirror)
# output: data/skins_full.json
# usage:  copy items_game.txt next to this script, then:  python gen_full_db.py
import re, json, os

SRC = "items_game.txt"
OUT = "data/skins_full.json"

src = open(SRC, encoding="utf-8").read()

# hero list — every playable hero key
HERO_RE = re.compile(r'"(npc_dota_hero_[\w]+)"')
all_heroes = sorted(set(HERO_RE.findall(src)))
# filter out base / unused keys
all_heroes = [h for h in all_heroes if not h.endswith("_base") and "target_dummy" not in h]
print(f"heroes found: {len(all_heroes)}")

def get_field(body, key):
    m = re.search(r'"%s"\s+"([^"]+)"' % re.escape(key), body)
    return m.group(1) if m else ""

def get_heroes(body):
    sec = re.search(r'"used_by_heroes"\s*\{(.*?)\}', body, re.S)
    if not sec:
        return []
    return re.findall(r'"(npc_dota_hero_\w+)"\s+"1"', sec.group(1))

# bundles / sets: "bundles" { "1234" { "name" ... } }
def get_bundles(src):
    out = {}
    for m in re.finditer(r'"bundles"\s*\{(.*?)\n\t\}', src, re.S):
        for b in re.finditer(r'"(\d+)"\s*\{(.*?)\}', m.group(1), re.S):
            bid, body = b.group(1), b.group(2)
            items_m = re.search(r'"items".*?\{(.*?)\}', body, re.S)
            items = re.findall(r'"(\d+)"', items_m.group(1)) if items_m else []
            out[bid] = {"name": get_field(body, "name"), "items": items}
    return out

bundles = get_bundles(src)
print(f"bundles found: {len(bundles)}")

# main items block
items_sec = re.search(r'"items"\s*\{(.*)\n\t\}\n\t"bundles"', src, re.S)
items_block = items_sec.group(1) if items_sec else src

skins = []
seen = set()

for m in re.finditer(r'"(\d+)"\s*\n?\s*\{(.*?)\n\t\t\}', items_block, re.S):
    defid, body = m.group(1), m.group(2)
    if defid in seen:
        continue
    seen.add(defid)

    name = get_field(body, "item_name") or get_field(body, "name")
    prefab = get_field(body, "prefab")
    slot = get_field(body, "item_slot") or get_field(body, "equipment_slot")
    rarity = get_field(body, "item_rarity") or "common"
    heroes = get_heroes(body)

    if not heroes and not slot:
        continue  # not a wearable, not a global — skip (emotes, music, etc. optional)

    # multi-hero items (e.g. all-hero belts) expand to each hero
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

# attach bundle membership so sets show as one-click equip
for bid, b in bundles.items():
    for item_def in b["items"]:
        for s in skins:
            if str(s["def"]) == item_def:
                s.setdefault("bundles", []).append({"id": int(bid), "name": b["name"]})

os.makedirs("data", exist_ok=True)
json.dump({"heroes": all_heroes, "skins": skins}, open(OUT, "w"), indent=1)
print(f"wrote {OUT}: {len(skins)} wearable entries across {len(all_heroes)} heroes")

# per-hero summary so she can see coverage
from collections import Counter
c = Counter(s["hero"] for s in skins)
for h in sorted(c):
    print(f"  {h}: {c[h]} items")
