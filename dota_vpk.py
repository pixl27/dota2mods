r"""
dota_vpk.py - locate the Dota 2 installation and read files straight out of
its pak01 VPK archive, so scripts never depend on files copied by hand.

usage: python dota_vpk.py ls <prefix>          list archive entries
       python dota_vpk.py cat <path> [out]     print or save one entry
       python dota_vpk.py where                print the detected game/dota directory

As a module: `game_directory()` returns <Steam>\steamapps\common\dota 2 beta\game\dota
and `Archive(game).read("scripts/items/items_game.txt")` returns bytes.
"""
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

GAME_RELATIVE = Path("steamapps") / "common" / "dota 2 beta" / "game" / "dota"


def _from_running_dota():
    try:
        output = subprocess.run(["wmic", "process", "where", "name='dota2.exe'", "get", "ExecutablePath"],
                                capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    for line in output.splitlines():
        line = line.strip()
        if line.lower().endswith("dota2.exe"):
            # <root>\game\bin\win64\dota2.exe -> <root>\game\dota
            candidate = Path(line).parent.parent.parent / "dota"
            if (candidate / "pak01_dir.vpk").exists():
                return candidate
    return None


def _steam_roots():
    roots = []
    try:
        import winreg
        for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
            try:
                with winreg.OpenKey(hive, key) as handle:
                    for name in ("SteamPath", "InstallPath"):
                        try:
                            roots.append(Path(winreg.QueryValueEx(handle, name)[0]))
                        except OSError:
                            pass
            except OSError:
                pass
    except ImportError:
        pass
    for root in list(roots):
        vdf = root / "steamapps" / "libraryfolders.vdf"
        if vdf.exists():
            text = vdf.read_text(encoding="utf-8", errors="replace")
            roots += [Path(p.replace("\\\\", "\\")) for p in re.findall(r'"path"\s+"([^"]+)"', text)]
    return roots


def game_directory(explicit=None):
    """Return Dota's game/dota directory, or None when it cannot be found."""
    if explicit:
        explicit = Path(explicit)
        for candidate in (explicit, explicit / "game" / "dota", explicit / "dota"):
            if (candidate / "pak01_dir.vpk").exists():
                return candidate
        return None
    if (found := _from_running_dota()):
        return found
    for root in _steam_roots():
        candidate = root / GAME_RELATIVE
        if (candidate / "pak01_dir.vpk").exists():
            return candidate
    return None


class Archive:
    """Minimal VPK v2 reader for pak01_dir.vpk (directory tree + numbered archives)."""

    def __init__(self, game):
        self.game = Path(game)
        self.directory = self.game / "pak01_dir.vpk"
        data = self.directory.read_bytes()
        signature, version, tree_size = struct.unpack_from("<III", data, 0)
        if signature != 0x55AA1234:
            raise ValueError(f"{self.directory} is not a VPK archive")
        self.header = 28 if version == 2 else 12
        self.tree_size = tree_size
        self.entries = {}
        at = self.header

        def string():
            nonlocal at
            end = data.index(b"\0", at)
            text = data[at:end].decode("utf-8", "replace")
            at = end + 1
            return text

        while True:
            extension = string()
            if not extension:
                break
            while True:
                path = string()
                if not path:
                    break
                while True:
                    name = string()
                    if not name:
                        break
                    crc, preload, archive, offset, length, terminator = struct.unpack_from("<IHHIIH", data, at)
                    at += 18
                    preload_bytes = data[at:at + preload]
                    at += preload
                    full = (path + "/" if path != " " else "") + name + "." + extension
                    self.entries[full] = (archive, offset, length, preload_bytes)
        self._directory_data = data

    def read(self, name):
        archive, offset, length, preload = self.entries[name]
        if archive == 0x7FFF:
            base = self.header + self.tree_size
            return preload + self._directory_data[base + offset:base + offset + length]
        with open(self.game / f"pak01_{archive:03d}.vpk", "rb") as handle:
            handle.seek(offset)
            return preload + handle.read(length)

    def names(self, prefix=""):
        return sorted(n for n in self.entries if n.startswith(prefix))


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    game = game_directory(os.environ.get("DOTA_GAME_DIR"))
    if not game:
        print("[!] Dota 2 not found: start Dota once, or set DOTA_GAME_DIR to <...>\\dota 2 beta\\game\\dota")
        return 2
    command = sys.argv[1]
    if command == "where":
        print(game)
        return 0
    archive = Archive(game)
    if command == "ls":
        for name in archive.names(sys.argv[2] if len(sys.argv) > 2 else ""):
            print(name)
        return 0
    if command == "cat" and len(sys.argv) > 2:
        content = archive.read(sys.argv[2])
        if len(sys.argv) > 3:
            Path(sys.argv[3]).write_bytes(content)
            print(f"[OK] {sys.argv[2]} -> {sys.argv[3]} ({len(content)} bytes)")
        else:
            sys.stdout.buffer.write(content)
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
