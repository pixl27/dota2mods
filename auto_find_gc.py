"""
auto_find_gc.py — Automated 1-click GC hook RVA finder for Wardrobe Level 3.
Zero external dependencies required (uses only standard Python libraries).

This script:
1. Automatically detects Steam and Dota 2 install paths across all drives.
2. Parses client.dll directly to find the GC cache dispatch function.
3. Automatically writes gc_hook.txt to build/ and C:\\Temp\\opencode\\.
"""

import os
import sys
import struct
import shutil

try:
    import winreg
except ImportError:
    winreg = None

def find_steam_libraries():
    """Locate all Steam library folders via Windows Registry and libraryfolders.vdf."""
    roots = set()
    
    # 1. Check Windows Registry
    if winreg:
        for hive, subkey in [
            (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
            (winreg.HKEY_LOCAL_MACHINE, r"Software\Valve\Steam"),
            (winreg.HKEY_LOCAL_MACHINE, r"Software\WOW6432Node\Valve\Steam"),
        ]:
            try:
                with winreg.OpenKey(hive, subkey) as k:
                    val, _ = winreg.QueryValueEx(k, "SteamPath")
                    if val and os.path.isdir(val):
                        roots.add(os.path.normpath(val))
            except OSError:
                pass

    # 2. Standard install locations across common drives
    common_roots = [
        r"C:\Program Files (x86)\Steam",
        r"C:\Program Files\Steam",
        r"C:\Steam",
        r"D:\Steam",
        r"D:\SteamLibrary",
        r"E:\Steam",
        r"E:\SteamLibrary",
        r"F:\SteamLibrary",
        r"G:\SteamLibrary",
    ]
    for c in common_roots:
        if os.path.isdir(c):
            roots.add(os.path.normpath(c))

    # 3. Parse libraryfolders.vdf for external drive libraries
    all_libs = set(roots)
    for r in list(roots):
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            try:
                with open(vdf, "r", encoding="utf-8", errors="ignore") as f:
                    for line in f:
                        line = line.strip()
                        if '"path"' in line:
                            parts = line.split('"')
                            if len(parts) >= 4:
                                p = os.path.normpath(parts[3])
                                if os.path.isdir(p):
                                    all_libs.add(p)
            except Exception:
                pass

    return list(all_libs)

def locate_client_dll(explicit_path=None):
    """Find client.dll automatically or verify user-provided path."""
    if explicit_path and os.path.isfile(explicit_path):
        return os.path.abspath(explicit_path)

    libraries = find_steam_libraries()
    for lib in libraries:
        cand = os.path.join(lib, "steamapps", "common", "dota 2 beta", "game", "bin", "win64", "client.dll")
        if os.path.isfile(cand):
            return os.path.abspath(cand)

    return None

def analyze_pe_for_gc_hook(dll_path):
    """Parses client.dll using pure Python struct to locate the GC dispatch RVA."""
    print(f"[*] Reading: {dll_path}")
    with open(dll_path, "rb") as f:
        data = f.read()

    file_len = len(data)
    if file_len < 0x200:
        raise ValueError("File too small to be a valid PE image.")

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    pe_sig = struct.unpack_from("<I", data, e_lfanew)[0]
    if pe_sig != 0x4550:
        raise ValueError("Invalid PE signature.")

    machine, num_sections = struct.unpack_from("<HH", data, e_lfanew + 4)
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt_header_offset = e_lfanew + 24
    opt_magic = struct.unpack_from("<H", data, opt_header_offset)[0]

    if opt_magic != 0x20B: # PE32+ (x64)
        raise ValueError("client.dll must be an x64 PE binary.")

    sec_table_offset = opt_header_offset + opt_size
    sections = []
    text_sec = None
    rdata_sec = None

    for i in range(num_sections):
        s_off = sec_table_offset + i * 40
        name = data[s_off:s_off+8].decode("latin1").rstrip("\x00")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s_off + 8)
        sec = {
            "name": name,
            "vsize": vsize,
            "vaddr": vaddr,
            "raw_size": raw_size,
            "raw_ptr": raw_ptr,
        }
        sections.append(sec)
        if name == ".text":
            text_sec = sec
        elif name == ".rdata":
            rdata_sec = sec

    if not text_sec or not rdata_sec:
        raise ValueError("Could not locate .text or .rdata sections.")

    print(f"[*] .text section: RVA 0x{text_sec['vaddr']:X}, size 0x{text_sec['vsize']:X}")
    print(f"[*] .rdata section: RVA 0x{rdata_sec['vaddr']:X}, size 0x{rdata_sec['vsize']:X}")

    # 1. Search for target string "SOCacheSubscribed" in .rdata
    target = b"SOCacheSubscribed"
    rdata_bytes = data[rdata_sec["raw_ptr"]:rdata_sec["raw_ptr"] + rdata_sec["raw_size"]]
    str_idx = rdata_bytes.find(target)

    if str_idx == -1:
        target = b"CMsgSOCacheSubscribed"
        str_idx = rdata_bytes.find(target)

    if str_idx == -1:
        raise ValueError("Target string 'SOCacheSubscribed' not found in client.dll.")

    str_rva = rdata_sec["vaddr"] + str_idx
    print(f"[+] Found {target.decode()} at RVA: 0x{str_rva:X}")

    # 2. Search .text for RIP-relative LEA referencing str_rva
    text_bytes = data[text_sec["raw_ptr"]:text_sec["raw_ptr"] + text_sec["raw_size"]]
    text_ptr = text_sec["raw_ptr"]
    text_vaddr = text_sec["vaddr"]
    text_len = len(text_bytes)

    xref_file_offset = None
    for i in range(text_len - 7):
        b0 = text_bytes[i]
        b1 = text_bytes[i + 1]
        if (b0 == 0x48 or b0 == 0x4C) and b1 == 0x8D:
            modrm = text_bytes[i + 2]
            if (modrm & 0xC7) == 0x05: # RIP-relative
                disp = struct.unpack_from("<i", text_bytes, i + 3)[0]
                inst_rva = text_vaddr + i
                target_rva = inst_rva + 7 + disp
                if target_rva == str_rva:
                    xref_file_offset = text_ptr + i
                    print(f"[+] Found RIP-relative xref in .text at RVA: 0x{inst_rva:X}")
                    break

    if xref_file_offset is None:
        raise ValueError("Could not find RIP-relative reference to target string.")

    xref_local = xref_file_offset - text_ptr

    # 2b. Check adjacent instructions (within 40 bytes) for another LEA pointing to code (callback pointer)
    start_scan = max(0, xref_local - 40)
    end_scan = min(text_len - 7, xref_local + 40)
    for i in range(start_scan, end_scan):
        if i == xref_local:
            continue
        b0 = text_bytes[i]
        b1 = text_bytes[i + 1]
        if (b0 == 0x48 or b0 == 0x4C) and b1 == 0x8D:
            modrm = text_bytes[i + 2]
            if (modrm & 0xC7) == 0x05:
                disp = struct.unpack_from("<i", text_bytes, i + 3)[0]
                tgt_rva = text_vaddr + i + 7 + disp
                if text_vaddr <= tgt_rva < text_vaddr + text_sec["vsize"]:
                    print(f"[+] Discovered handler callback pointer at RVA: 0x{tgt_rva:X}")
                    return tgt_rva

    # 3. Fallback: trace backward to function prologue
    print("[*] Tracing backward for function prologue...")
    prologue_rva = None
    for i in range(xref_local, max(0, xref_local - 0x1000), -1):
        prev = text_bytes[i - 1] if i > 0 else 0
        if prev in (0xCC, 0xC3, 0x90):
            c0 = text_bytes[i]
            c1 = text_bytes[i + 1]
            c2 = text_bytes[i + 2] if i + 2 < text_len else 0
            if (c0 == 0x48 and c1 == 0x89 and c2 == 0x5C) or \
               (c0 == 0x48 and c1 == 0x83 and c2 == 0xEC) or \
               (c0 == 0x48 and c1 == 0x81 and c2 == 0xEC) or \
               (c0 == 0x40 and c1 in (0x53, 0x55, 0x57)) or \
               (c0 == 0x55 and c1 == 0x48 and c2 in (0x89, 0x8B)):
                prologue_rva = text_vaddr + i
                break

    if prologue_rva:
        print(f"[+] Discovered function entry prologue at RVA: 0x{prologue_rva:X}")
        return prologue_rva

    raise ValueError("Could not determine function entry RVA.")

def main():
    print("=" * 65)
    print("      Wardrobe Level 3 - Automated GC Hook RVA Finder")
    print("=" * 65)

    arg_path = sys.argv[1] if len(sys.argv) > 1 else None
    dll_path = locate_client_dll(arg_path)

    if not dll_path:
        print("\n[!] Could not locate client.dll automatically.")
        print("    Usage: python auto_find_gc.py \"<path to client.dll>\"")
        print("    Example: python auto_find_gc.py \"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta\\game\\bin\\win64\\client.dll\"")
        sys.exit(1)

    print(f"[OK] Located Dota 2 client.dll:\n     {dll_path}\n")

    try:
        rva = analyze_pe_for_gc_hook(dll_path)
    except Exception as e:
        print(f"\n[!] Error during analysis: {e}")
        sys.exit(1)

    hex_rva = f"{rva:X}"
    print(f"\n" + "-" * 65)
    print(f" [SUCCESS] GC Hook RVA: 0x{hex_rva}")
    print("-" * 65)

    # Save to outputs
    dest_dirs = ["build", ".", "C:\\Temp\\opencode"]
    for d in dest_dirs:
        try:
            os.makedirs(d, exist_ok=True)
            out_file = os.path.join(d, "gc_hook.txt")
            with open(out_file, "w", encoding="utf-8") as f:
                f.write(f"{hex_rva}\n")
            print(f"[OK] Saved: {out_file}")
        except Exception as ex:
            pass

    # Copy skins_full.json if available
    db_sources = ["data/skins_full.json", "skins_full.json", "build/data/skins_full.json"]
    for src in db_sources:
        if os.path.isfile(src):
            for d in ["build/data", "C:\\Temp\\opencode"]:
                try:
                    os.makedirs(d, exist_ok=True)
                    dest = os.path.join(d, "skins_full.json")
                    shutil.copyfile(src, dest)
                    print(f"[OK] Synced database: {dest}")
                except Exception:
                    pass
            break

    print("\n[DONE] Level 3 is fully configured! Launch Dota 2 and run map.exe.")

if __name__ == "__main__":
    main()
