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
2. pip install vdf
3. python update_db.py          -> data/skins_full.json (~14k entries)
   It reads items_game.txt and both localization files straight from Dota's
   pak01 VPK (Dota installed, or running), so nothing is copied by hand.
   Add --all-global to also list courier/ward/creep cosmetics that a local
   server never renders. Re-run it after every Dota update, then build.bat.
4. Open "x64 Native Tools Command Prompt for VS", cd here, run build.bat
     -> build\wardrobe.exe + build\wardrobe_dll.dll + build\map.exe
5. Copy cfg\gamestate_integration_wardrobe.cfg into:
     Steam\steamapps\common\dota 2 beta\game\dota\cfg\gamestate_integration\
   (create the folder if missing)

OVERLAY ONLY (wardrobe preview + loadout staging):
1. Start Dota 2.
2. Run build\wardrobe.exe. No admin. No injection.
3. Overlay appears. END toggles it (INSERT belongs to the in-game HUD).
4. Left column: pick hero. Right: open slot, click skin, equipped.
   "wear: <set>" = whole outfit in one click.
5. Loadout auto-saves to loadout.json.

EQUIPEMENT NATIF EN PARTIE (v6, apparence locale):
1. Lance Dota, puis launch_level3.bat une fois pour cette session.
2. Equipe les objets dans le menu natif de Dota.
3. Choisis ton heros en partie locale. La tenue est transmise au moteur
   automatiquement, avec regroupement des clics rapides.
4. INSERT affiche l'etat de l'inventaire et celui de l'apparence en partie.
   Aucun offsets.bin, second navigateur de tenues ou re-push manuel requis.

NOTES:
- GC inventory + local equip receiver v6: restart Dota if an older DLL was loaded, then run
  launch_level3.bat (or build\launch_level3.bat) once and press INSERT in Dota.
  It requests a fresh inventory automatically.
  After 20 seconds without a usable reply, the HUD shows the reason and a
  "Retry inventory refresh" button. Log: C:\Temp\opencode\wardrobe_gc.log.
  Native appearance uses a verified client.dll profile (see docs/native-appearance.md).
  Personas (e.g. Davion of Dragon Hold) swap the hero skeleton; models the server
  never precached are registered in Dota's just-in-time manifest before loading,
  otherwise the engine draws models/dev/error.vmdl. INSERT shows the model Dota
  actually renders two seconds after each outfit update.
  Equip items in Dota's native loadout screen. The receiver stays active after
  refresh; INSERT shows accepted/completed equips, pending replies, and latency.
  Receiver v4 notifies Dota immediately when a local reply is ready, preserves
  real GC traffic priority, and flushes diagnostic logs on its worker thread.
  Dota can save preview items in cache_<account>_1.soc; they may remain visible
  after restarting Dota without Wardrobe. This does not mean the DLL autostarts.
  To restore the saved Steam cache, close Dota completely and run:
    python repair_inventory_cache.py "<Dota installation>\game\dota" --repair
  Only caches containing Wardrobe IDs are backed up and removed; Steam reloads
  them at the next connection. Backups are kept in %LOCALAPPDATA%\Wardrobe\recovery;
  --list-backups shows them and --restore latest puts one back (Dota closed).
  Receiver v6 isolates econ service 1, prevents duplicate loading, and resumes
  an already delivered inventory without reloading the full cosmetic catalog.
- Regression checks: tests\run_tests.bat. Noninteractive build: build.bat --no-pause.
- After a Dota update: python verify_profile.py tells whether client.dll still matches
  the appearance profile (launch_level3.bat runs it automatically when Python exists).
- Logs in C:\Temp\opencode rotate to *.old once they pass 4 MB.
- The preview overlay uses Game State Integration telemetry. Global cosmetics
  (loading screens, HUD skins, music, terrains, cursor packs, announcers) are in
  the catalog and equip natively; they are client-side in Dota, so they should
  apply in local lobbies, but that path has not been verified in game yet.
- wardrobe_dll.dll = live swap. map.exe ONLY — never double-click the DLL,
  never LoadLibrary it. A changed Dota binary disables the native appearance profile.
- Streamproof is ON: OBS/Discord won't capture the menu.
- The local hero game thread handles appearance updates; the old keeper/TCP writer was removed from the DLL.
