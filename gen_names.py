# gen_names.py — tokens -> english names
# input:  resource/localization/items_english.txt + data/skins_full.json
# usage:  copy items_english.txt next to this script, then:  python gen_names.py
import re, json, os, sys

if not os.path.exists("items_english.txt"):
    print("[!] items_english.txt not found. Copy items_english.txt next to this script.")
    sys.exit(0)

lang = open("items_english.txt", encoding="utf-8", errors="ignore").read()
# Match any key-value pair of quoted strings
raw_tokens = re.findall(r'"([^"]+)"\s+"([^"]+)"', lang)
tokens = {}
for k, v in raw_tokens:
    tokens[k.lower().lstrip("#")] = v

print(f"tokens loaded: {len(tokens)}")

db_path = "data/skins_full.json"
if not os.path.exists(db_path):
    print(f"[!] {db_path} not found. Run gen_full_db.py first.")
    sys.exit(0)

db = json.load(open(db_path, encoding="utf-8"))
fixed = 0
for s in db.get("skins", []):
    name = s.get("name", "")
    lookup = name.lower().lstrip("#")
    if lookup in tokens:
        s["name"] = tokens[lookup]
        fixed += 1

with open(db_path, "w", encoding="utf-8") as f:
    json.dump(db, f, indent=1)

print(f"names resolved: {fixed}")
