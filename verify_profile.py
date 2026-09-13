r"""
verify_profile.py - checks that Dota's client.dll still matches the verified
native appearance profile (src/native_appearance_profile.h).

usage: python verify_profile.py [path\to\client.dll]

Without an argument the script locates client.dll from the running dota2.exe,
then from the Steam library folders. Exit code 0 = match, 1 = mismatch
(Dota updated: in-game appearance is disabled until the profile is re-verified),
2 = client.dll or the profile could not be found.
"""
import hashlib, os, re, sys

def profile_hash():
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(here, "src", "native_appearance_profile.h"),
                      os.path.join(here, "..", "src", "native_appearance_profile.h")):
        if os.path.exists(candidate):
            match = re.search(r'Sha256\[\]\s*=\s*"([0-9a-f]{64})"', open(candidate, encoding="utf-8").read())
            if match: return match.group(1)
    return None

def from_running_dota():
    """The running game's client.dll, via dota_vpk's process lookup."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import dota_vpk
        game = dota_vpk.game_directory()
    except Exception:
        return None
    return os.path.join(game, "bin", "win64", "client.dll") if game else None


def from_steam_libraries():
    roots = []
    try:
        import winreg
        for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"), (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    for value in ("SteamPath", "InstallPath"):
                        try: roots.append(winreg.QueryValueEx(k, value)[0])
                        except OSError: pass
            except OSError: pass
    except ImportError:
        pass
    libraries = list(roots)
    for root in roots:
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if os.path.exists(vdf):
            libraries += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="replace").read())]
    for library in libraries:
        candidate = os.path.join(library, "steamapps", "common", "dota 2 beta", "game", "dota", "bin", "win64", "client.dll")
        if os.path.exists(candidate): return candidate
    return None

def main():
    expected = profile_hash()
    if not expected:
        print("[!] native_appearance_profile.h not found next to this script"); return 2
    path = sys.argv[1] if len(sys.argv) > 1 else (from_running_dota() or from_steam_libraries())
    if not path or not os.path.exists(path):
        print("[!] client.dll not found; pass its path explicitly"); return 2
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): digest.update(chunk)
    actual = digest.hexdigest()
    if actual == expected:
        print(f"[OK] {path} is exactly the build the appearance profile was recorded from"); return 0
    print(f"[!] {path} is not the build the profile was recorded from.")
    print(f"    expected {expected}\n    actual   {actual}")
    print("    This is normal after a Dota update and usually needs nothing: the DLL re-locates")
    print("    every function and offset in the new binary on its own, and the INSERT panel says")
    print("    whether it did. Run  python refresh_profile.py --check  for the detailed answer,")
    print("    and  python refresh_profile.py  (then build.bat) if it reports an unresolved entry.")
    return 1

if __name__ == "__main__":
    sys.exit(main())
