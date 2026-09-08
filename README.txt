WARDROBE — how to use, baby
===========================

FIRST TIME (10 min, once):
1. Third-party libs (into thirdparty\):
     - imgui:      https://github.com/ocornut/imgui (zip, extract so that
                   thirdparty\imgui\imgui.cpp exists, plus backends\imgui_impl_win32.cpp
                   and backends\imgui_impl_dx11.cpp)
     - minhook:    https://github.com/TsudaKageyu/minhook (extract so that
                   thirdparty\minhook\include\MinHook.h exists)
     - json.hpp:   https://github.com/nlohmann/json/releases (single header,
                   save as thirdparty\json.hpp)
2. Copy items_game.txt + items_english.txt next to gen_full_db.py:
     Steam\steamapps\common\dota 2 beta\game\dota\scripts\items\items_game.txt
     Steam\steamapps\common\dota 2 beta\game\dota\resource\localization\items_english.txt
3. python gen_full_db.py        -> data/skins_full.json  (~12k entries)
   python gen_names.py          -> real english names
4. Open "x64 Native Tools Command Prompt for VS", cd here, run build.bat
     -> build\wardrobe.exe + build\wardrobe_dll.dll + build\map.exe
5. Copy cfg\gamestate_integration_wardrobe.cfg into:
     Steam\steamapps\common\dota 2 beta\game\dota\cfg\gamestate_integration\
   (create the folder if missing)

OVERLAY ONLY (undetectable, wardrobe preview + loadout staging):
1. Start Dota 2.
2. Run build\wardrobe.exe. No admin. No injection.
3. Overlay appears. INSERT toggles it.
4. Left column: pick hero. Right: open slot, click skin, equipped.
   "wear: <set>" = whole outfit in one click.
5. Loadout auto-saves to loadout.json.

LIVE MODEL SWAP (lobby sees it):
1. python dump_offsets.py "<path>\client.dll"   (once per patch, confirm + --write)
   -> offsets.bin  (copy next to wardrobe_dll.dll AND C:\Temp\opencode\)
2. Start Dota 2 (client.dll must be loaded — pick phase or menu is fine).
3. build\map.exe build\wardrobe_dll.dll   (maps the writer — 2 seconds)
4. build\wardrobe.exe                     (same browser as before)
5. Click any skin -> pushed live instantly. INSERT in-game = status menu.

NOTES:
- Overlay alone never touches Dota. GSI is Valve-blessed telemetry. Zero surface.
- wardrobe_dll.dll = live swap. map.exe ONLY — never double-click the DLL,
  never LoadLibrary it. Needs fresh offsets per patch. Off = safe.
- Streamproof is ON: OBS/Discord won't capture the menu.
- Death/respawn wipes wearables — keeper thread re-pushes automatically.
- Don't swap on your main the day of a VAC wave. Smurf first, main later.
