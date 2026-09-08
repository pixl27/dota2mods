r"""
dump_offsets.py — finds wearable offsets in client.dll, writes offsets.bin
needs: pip install pefile
usage: python dump_offsets.py "<path to>\client.dll"
       (Steam\steamapps\common\dota 2 beta\game\bin\win64\client.dll)
"""
import struct, sys

if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
    print(__doc__)
    sys.exit(0)

if sys.argv[1] == "--write":
    vals = [int(x, 16) for x in sys.argv[2:10]]
    if len(vals) != 8:
        print("[!] need exactly 8 hex numbers:")
        print("    dwLocalPlayerHero m_hWearables m_ItemView m_iItemDefIndex")
        print("    m_nFallbackPaint m_bNeedReapply dwEntityList fnFullUpdate")
        sys.exit(1)
    blob = struct.pack("<II8Q",
        0x57415244, 1,
        vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6], vals[7])
    open("offsets.bin", "wb").write(blob)
    print("[OK] offsets.bin written. Copy it next to wardrobe_dll.dll and to C:\\Temp\\opencode\\")
    sys.exit(0)

path = sys.argv[1]
img = open(path, "rb").read()

print("== wardrobe offset dumper ==")
print("client.dll size:", len(img))
for s in [b"C_EconWearable", b"C_DOTA_BaseNPC_Hero", b"m_iItemDefinitionIndex",
         b"SOCacheSubscribed", b"CGCClient", b"ClientWelcome"]:
    at = img.find(s)
    print(f"  string {s!r}: {'FOUND @ ' + hex(at) if at > 0 else 'missing'}")

print("""
Manual confirm (once per patch, 10 min in IDA/Ghidra free):
 1. Open client.dll, find C_DOTA_BaseNPC_Hero vtable.
 2. m_hWearables  = first EHANDLE array member (8 x uint32).
 3. C_EconWearable -> member pointing at CEconItemView struct = m_ItemView.
 4. CEconItemView + small int written right after equip event = m_iItemDefIndex.
 5. Hero bool set true in WearWearables() = m_bNeedReapply.
 6. dwLocalPlayerHero / dwEntityList = standard interface walk (VClientEntityList).
 7. fnFullUpdate = hero full-update / force-redraw fn.

Then run:  python dump_offsets.py --write <8 hex numbers...>
Order: dwLocalPlayerHero m_hWearables m_ItemView m_iItemDefIndex
       m_nFallbackPaint m_bNeedReapply dwEntityList fnFullUpdate

GC hook site (once per patch, xrefs of SOCacheSubscribed in client.dll):
 1. Find string "SOCacheSubscribed" -> xref to OnSOCacheSubscribed handler.
 2. Its 3rd caller up is the GC dispatch (switch on EMsg). Hook address = that caller.
 3. Save the RVA (address minus client.dll base) as hex into gc_hook.txt:
      echo 1A2B3C4 > gc_hook.txt
    Copy gc_hook.txt next to wardrobe_dll.dll and to C:\\Temp\\opencode\\
""")
