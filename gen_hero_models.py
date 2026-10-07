r"""
gen_hero_models.py - writes src/hero_models.h: each hero's classic model, for
every cosmetic that replaces a hero model the model it replaces, and the
animation (activity) modifiers each cosmetic adds.

usage: python gen_hero_models.py [--dota <...\dota 2 beta\game\dota>] [--out PATH] [--check]

All tables come straight from the game's pak01 VPK:
- scripts/npc/heroes/npc_dota_hero_*.txt gives HeroID and Model;
- scripts/items/items_game.txt gives the asset_modifier blocks of type
  "entity_model" (asset = the hero's npc name) and "hero_model_change"
  (asset = the model of an alternate form, such as Elder Dragon Form), and
  each item's "activity" blocks (asset = ALL or one ACT_*, an optional style).

Why the DLL needs them:
- the classic model must never be learned at run time: a hero that spawned, or
  was first seen, in an alternate form (Dragon Knight as a dragon) otherwise
  records that form as the model to restore, and every later return to human
  is mistaken for a transformation;
- the server keeps animating the model it knows. Translating its animation
  choice onto the replacement needs the model the server is actually using;
- the server also never hears of the outfit's items, so it never asks for the
  animations they unlock (a peg leg's run, a spear's attacks). The DLL adds
  those modifiers when it picks the sequence itself.

A hero or cosmetic added after the last regeneration simply has no entry, and
the DLL falls back to what it observes. --check only reports whether the
header is current. update_db.py runs this script.
"""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dota_vpk  # noqa: E402

HERO_FILE = re.compile(r"^scripts/npc/heroes/npc_dota_hero_[a-z0-9_]+\.txt$")
# asset_modifier blocks are flat key/value lists; nested braces never occur inside one.
MODIFIER_BLOCK = re.compile(r'"asset_modifier"\s*\{([^{}]*)\}')


def field(block, key):
    match = re.search(r'"' + key + r'"\s+"([^"]*)"', block)
    return match.group(1) if match else None


def hero_models(archive):
    """{hero id: (npc name, classic model)} from the per-hero npc scripts."""
    heroes = {}
    for name in archive.names("scripts/npc/heroes/"):
        if not HERO_FILE.match(name):
            continue
        text = archive.read(name).decode("utf-8", "replace")
        npc = Path(name).stem
        # The hero block is the first one; abilities and talents never carry HeroID.
        hero_id, model = field(text, "HeroID"), field(text, "Model")
        if hero_id and model and hero_id.isdigit():
            heroes[int(hero_id)] = (npc, model.replace("\\", "/").lower())
    return heroes


def model_origins(text, heroes):
    """{replacement model: original model} for every hero-model replacement."""
    by_npc = {npc: model for npc, model in heroes.values()}
    origins, conflicts = {}, []
    for block in MODIFIER_BLOCK.findall(text):
        kind = field(block, "type")
        asset, modifier = field(block, "asset"), field(block, "modifier")
        if not asset or not modifier or not modifier.endswith(".vmdl"):
            continue
        if kind == "entity_model":
            original = by_npc.get(asset.lower())
        elif kind == "hero_model_change":
            original = asset.replace("\\", "/").lower() if asset.endswith(".vmdl") else None
        else:
            continue
        if not original:
            continue
        replacement = modifier.replace("\\", "/").lower()
        if replacement == original:
            continue
        previous = origins.setdefault(replacement, original)
        if previous != original:
            conflicts.append((replacement, previous, original))
    return origins, conflicts


def item_activities(text):
    """[(definition, style or -1, activity, modifier)] from each item's visuals."""
    try:
        import vdf
    except ImportError:
        print("[!] the vdf package is required: pip install vdf")
        sys.exit(1)
    # VDFDict keeps duplicate keys such as repeated asset_modifier blocks.
    root = vdf.loads(text, mapper=vdf.VDFDict)
    items = root.get("items_game", root).get("items", {})
    rows = set()
    for key, item in items.items():
        definition = key[1] if isinstance(key, tuple) else key
        visuals = item.get("visuals") if isinstance(item, dict) else None
        if not str(definition).isdigit() or not isinstance(visuals, dict):
            continue
        for _, block in visuals.items():
            if not isinstance(block, dict) or block.get("type") != "activity":
                continue
            activity, modifier, style = block.get("asset"), block.get("modifier"), block.get("style")
            if not isinstance(activity, str) or not isinstance(modifier, str) or not activity or not modifier:
                continue
            style = int(style) if isinstance(style, str) and style.isdigit() else -1
            rows.add((int(definition), style, activity, modifier))
    return sorted(rows)


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(heroes, origins, activities):
    lines = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <cstring>",
        "",
        "// Generated by gen_hero_models.py from the game's VPK. Do not edit.",
        "// HeroModels: each hero's classic model, the one the server networks.",
        "// ModelOrigins: for each cosmetic model that replaces a hero model (an arcana,",
        "// a persona, a persona's alternate form), the model it replaces.",
        "namespace appearance::catalog {",
        "struct HeroModel { uint16_t hero; const char* model; };",
        "struct ModelOrigin { const char* replacement; const char* original; };",
        "struct ItemActivity { uint32_t definition; int32_t style; const char* activity; const char* modifier; };",
        "inline constexpr HeroModel HeroModels[] = {",
    ]
    for hero_id in sorted(heroes):
        lines.append(f'    {{{hero_id}, "{heroes[hero_id][1]}"}},')
    lines += ["};", "inline constexpr ModelOrigin ModelOrigins[] = {"]
    for replacement in sorted(origins):
        lines.append(f'    {{"{replacement}", "{origins[replacement]}"}},')
    lines += ["};",
              "// ItemActivities: the activity modifiers an item adds, sorted by definition. style -1",
              "// applies to every style; activity is \"ALL\" or the one ACT_* it applies to.",
              "inline constexpr ItemActivity ItemActivities[] = {"]
    for definition, style, activity, modifier in activities:
        lines.append(f"    {{{definition}, {style}, {c_string(activity)}, {c_string(modifier)}}},")
    lines += [
        "};",
        "// The entries of one definition, by binary search over the sorted table.",
        "inline const ItemActivity* ItemActivitiesFor(uint32_t definition, size_t& count) {",
        "    size_t low = 0, high = sizeof(ItemActivities) / sizeof(ItemActivities[0]);",
        "    while (low < high) {",
        "        const size_t middle = (low + high) / 2;",
        "        if (ItemActivities[middle].definition < definition) low = middle + 1; else high = middle;",
        "    }",
        "    size_t end = low;",
        "    while (end < sizeof(ItemActivities) / sizeof(ItemActivities[0]) && ItemActivities[end].definition == definition) ++end;",
        "    count = end - low;",
        "    return count ? &ItemActivities[low] : nullptr;",
        "}",
        "inline const char* ClassicModel(uint32_t hero) {",
        "    for (const auto& entry : HeroModels) if (entry.hero == hero) return entry.model;",
        "    return nullptr;",
        "}",
        "// Exact path only: combined meshes (\"<base>_c_<n>.vmdl\") are matched by the caller.",
        "inline const char* OriginalModel(const char* replacement) {",
        "    if (!replacement) return nullptr;",
        "    for (const auto& entry : ModelOrigins) if (!_stricmp(entry.replacement, replacement)) return entry.original;",
        "    return nullptr;",
        "}",
        "// An original that is not any hero's classic model is an alternate form",
        "// (models/heroes/dragon_knight/dragon_knight_dragon.vmdl): never a baseline.",
        "inline bool AlternateForm(const char* model) {",
        "    if (!model || !*model) return false;",
        "    for (const auto& entry : HeroModels) if (!_stricmp(entry.model, model)) return false;",
        "    for (const auto& entry : ModelOrigins) if (!_stricmp(entry.original, model)) return true;",
        "    return false;",
        "}",
        "}",
    ]
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dota", type=Path, default=None, help="Dota's game/dota directory (auto-detected by default)")
    parser.add_argument("--out", type=Path, default=HERE / "src" / "hero_models.h")
    parser.add_argument("--check", action="store_true", help="report whether the header is current; write nothing")
    args = parser.parse_args(argv)
    game = dota_vpk.game_directory(args.dota)
    if not game:
        print("[!] Dota 2 not found: start Dota once, or pass --dota <...>\\dota 2 beta\\game\\dota")
        return 2
    archive = dota_vpk.Archive(game)
    heroes = hero_models(archive)
    if len(heroes) < 100:
        print(f"[!] only {len(heroes)} heroes found in scripts/npc/heroes: the VPK layout changed?")
        return 1
    items_game = archive.read("scripts/items/items_game.txt").decode("utf-8", "replace")
    origins, conflicts = model_origins(items_game, heroes)
    activities = item_activities(items_game)
    for replacement, first, other in conflicts:
        print(f"[*] {replacement} replaces both {first} and {other}; keeping the first")
    text = render(heroes, origins, activities)
    current = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
    print(f"[*] {len(heroes)} heroes, {len(origins)} model replacements, {len(activities)} item activity modifiers")
    if args.check:
        print("[OK] src/hero_models.h is current" if current == text else "[!] src/hero_models.h is out of date: run gen_hero_models.py")
        return 0 if current == text else 1
    if current != text:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8", newline="\n")
        print(f"[OK] wrote {args.out}; run build.bat")
    else:
        print(f"[OK] {args.out} already current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
