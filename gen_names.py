# gen_names.py — tokens -> english names
# input:  resource/localization/items_english.txt + data/skins_full.json
# usage:  copy items_english.txt next to this script, then:  python gen_names.py
import re, json

lang = open("items_english.txt", encoding="utf-8").read()
tokens = dict(re.findall(r'"(#DOTA_Item_[^"]+)"\s+"([^"]+)"', lang))
print(f"tokens loaded: {len(tokens)}")

db = json.load(open("data/skins_full.json"))
fixed = 0
for s in db["skins"]:
    if s["name"].startswith("#") and s["name"] in tokens:
        s["name"] = tokens[s["name"]]
        fixed += 1
json.dump(db, open("data/skins_full.json", "w"), indent=1)
print(f"names resolved: {fixed}")
