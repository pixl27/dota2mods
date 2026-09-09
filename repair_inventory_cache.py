"""Inspect Dota's serialized inventory caches; back up affected files only.

Schema: CMsgSerializedSOCache in SteamDatabase/Protobufs/dota2/gcsdk_gcmessages.proto.
Default is read-only. --repair requires Dota to be closed and moves caches with
Wardrobe's generated IDs into a timestamped backup, forcing a fresh Steam cache.
Backups live under %LOCALAPPDATA%\Wardrobe\recovery (never inside build\).
--list-backups shows them; --restore <timestamp|latest> copies a backup back,
hash-verified, moving any cache Steam recreated since aside in the same folder.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def default_backup_directory():
    root = os.environ.get("LOCALAPPDATA")
    return (Path(root) if root else Path(__file__).parent) / "Wardrobe" / "recovery"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def list_backups(directory):
    if not directory.exists():
        return []
    return sorted(p for p in directory.iterdir() if p.is_dir() and (p / "manifest.json").exists())


def restore(parser, game, directory, which):
    backups = list_backups(directory)
    if not backups:
        parser.error(f"no backups under {directory}")
    chosen = backups[-1] if which == "latest" else directory / which
    if chosen not in backups:
        parser.error(f"unknown backup {which}; use --list-backups")
    if dota_running():
        parser.error("close Dota completely before restoring its inventory cache")
    manifest = json.loads((chosen / "manifest.json").read_text(encoding="utf-8"))
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    for report in manifest:
        source = chosen / report["file"]
        if not source.exists() or sha256(source) != report["sha256"]:
            raise RuntimeError(f"backup {source.name} is missing or corrupted; nothing restored")
    for report in manifest:
        source, target = chosen / report["file"], game / report["file"]
        if target.exists():
            aside = chosen / f"{target.name}.replaced-{stamp}"
            shutil.move(str(target), str(aside))
            print(f"Moved the cache Steam recreated aside: {target.name} -> {aside}")
        shutil.copy2(source, target)
        if sha256(target) != report["sha256"]:
            raise RuntimeError(f"verification failed after copying {target.name}")
        print(f"Restored {target.name} from {chosen.name}")


def fields(data):
    at = 0

    def varint():
        nonlocal at
        value = 0
        for shift in range(0, 70, 7):
            if at >= len(data):
                raise ValueError("truncated varint")
            byte = data[at]
            at += 1
            if shift == 63 and byte > 1:
                raise ValueError("overflowing varint")
            value |= (byte & 127) << shift
            if not byte & 128:
                return value
        raise ValueError("invalid varint")

    result = []
    while at < len(data):
        tag = varint()
        number, wire = tag >> 3, tag & 7
        if not number:
            raise ValueError("invalid field")
        if wire == 0:
            value = varint()
        elif wire in (1, 2, 5):
            length = varint() if wire == 2 else 8 if wire == 1 else 4
            if length > len(data) - at:
                raise ValueError("truncated field")
            value = data[at:at + length]
            at += length
            if wire != 2:
                value = int.from_bytes(value, "little")
        else:
            raise ValueError("unsupported wire type")
        result.append((number, wire, value))
    return result


def value(parsed, number, default=0):
    return next((v for n, w, v in reversed(parsed) if n == number and w != 2), default)


def inspect(path, definitions):
    data = path.read_bytes()
    if len(data) > 64 * 1024 * 1024:
        raise ValueError("cache exceeds the inspection limit")
    parsed = fields(data)
    if value(parsed, 1) != 4:
        raise ValueError("unsupported serialized cache version")
    groups = []
    for n, w, body in parsed:
        if (n, w) != (2, 2):
            continue
        cache = fields(body)
        account = value(cache, 2) & 0xffffffff
        for n, w, group in cache:
            if (n, w) != (4, 2):
                continue
            group = fields(group)
            if value(group, 1) != 1:
                continue
            total = generated = 0
            generations = set()
            for n, w, item in group:
                if (n, w) != (2, 2):
                    continue
                item = fields(item)
                total += 1
                item_id, definition = value(item, 1), value(item, 4)
                # Match our ID namespace, account and actual shipped definitions.
                if item_id >> 62 == 1 and value(item, 2) == account and definition in definitions:
                    generated += 1
                    generations.add(item_id >> 24)
            groups.append(dict(service=value(group, 3), items=total,
                               wardrobe_items=generated, generations=len(generations)))
    return dict(file=path.name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                groups=groups, wardrobe_items=sum(g["wardrobe_items"] for g in groups))


def dota_running():
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq dota2.exe", "/FO", "CSV", "/NH"],
                            check=True, capture_output=True, text=True).stdout
    return '"dota2.exe"' in output.lower()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_directory", type=Path, help="Dota's game/dota directory")
    parser.add_argument("--catalog", type=Path, default=Path(__file__).parent / "data/skins_full.json")
    parser.add_argument("--backup-directory", type=Path, default=default_backup_directory())
    parser.add_argument("--repair", action="store_true")
    parser.add_argument("--list-backups", action="store_true")
    parser.add_argument("--restore", metavar="TIMESTAMP", help="copy a backup back (or 'latest')")
    args = parser.parse_args()
    game = args.game_directory.resolve(strict=True)
    if game.name != "dota" or game.parent.name != "game":
        parser.error("expected the specific Dota game/dota directory")
    if args.list_backups:
        for backup in list_backups(args.backup_directory.resolve()):
            files = [p.name for p in backup.iterdir() if p.suffix == ".soc"]
            print(f"{backup.name}: {', '.join(files) or '(empty)'}")
        return
    if args.restore:
        restore(parser, game, args.backup_directory.resolve(), args.restore)
        return
    definitions = {int(skin["def"]) for skin in json.loads(args.catalog.read_text(encoding="utf-8"))["skins"]}
    reports = []
    for path in sorted(game.glob("cache_*_1.soc")):
        try:
            report = inspect(path, definitions)
        except ValueError as error:
            report = dict(file=path.name, error=str(error), wardrobe_items=0)
        reports.append(report)
    print(json.dumps(reports, indent=2))
    affected = [r for r in reports if r["wardrobe_items"]]
    if not args.repair or not affected:
        return
    if dota_running():
        parser.error("close Dota completely before repairing its inventory cache")
    backup = args.backup_directory.resolve() / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup.mkdir(parents=True, exist_ok=False)
    (backup / "manifest.json").write_text(json.dumps(affected, indent=2), encoding="utf-8")
    for report in affected:
        source = (game / report["file"]).resolve(strict=True)
        if source.parent != game or dota_running():
            raise RuntimeError("cache location changed or Dota started; repair stopped")
        if hashlib.sha256(source.read_bytes()).hexdigest() != report["sha256"]:
            raise RuntimeError("cache changed since inspection; repair stopped")
        destination = backup / source.name
        shutil.copy2(source, destination)
        if hashlib.sha256(destination.read_bytes()).hexdigest() != report["sha256"]:
            raise RuntimeError("backup verification failed; original preserved")
        source.unlink()
        print(f"Backed up affected cache: {source.name} -> {destination}")


if __name__ == "__main__":
    main()
