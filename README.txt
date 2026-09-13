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

USAGE (tout se fait depuis l'application):
1. Lance build\Wardrobe.exe. Pas d'admin, pas d'installation.
2. L'ecran d'accueil dit ou en est chaque element et propose la seule action
   utile du moment : lancer Dota 2, puis activer Wardrobe.
3. Equipe tes objets dans le menu natif de Dota, puis lance une partie.
4. INSERT en jeu ouvre le panneau de diagnostic de la DLL.

L'onglet Entretien regroupe ce qui etait dans les .bat et les scripts Python :
mise a jour du catalogue, verification et regeneration du profil client.dll,
recompilation, restauration du cache Steam. L'onglet Journal montre en clair ce
que chaque operation a fait, avec la commande exacte qui a ete lancee.

L'activation n'est declaree reussie que lorsque l'application constate
elle-meme que la bibliotheque est chargee dans le jeu, jamais sur la seule
foi du code de retour du chargeur.

launch_level3.bat reste disponible pour qui prefere la ligne de commande.
L'ancien navigateur de tenues (wardrobe.exe) n'est plus construit : son
loadout.json n'etait lu par personne, l'equipement passant par le menu de Dota.

DONNER LE TOUT A QUELQU'UN:
1. build.bat, puis  python package.py
2. Envoie le fichier build\Wardrobe-<date>.zip (environ 1,4 Mo).
3. En face : decompresser, lancer Wardrobe.exe. Rien a installer, pas d'admin.
   Les binaires n'importent que des DLL Windows : aucun redistribuable
   Visual C++ n'est necessaire, et package.py refuse de packager si ce
   n'etait plus vrai.
   Windows SmartScreen avertira (programme non signe) et l'antivirus peut
   reagir : le programme charge du code dans Dota 2.
   Dota 2 est protege par VAC. Charger du code dans le jeu est exactement ce
   qu'un anti-triche cherche ; un compte peut etre sanctionne. Le LISEZMOI.txt
   inclus dans le paquet le dit noir sur blanc.
4. Le paquet embarque le catalogue et le profil client.dll. Si le Dota d'en
   face est d'une autre version, la DLL relocalise le profil toute seule au
   chargement ; c'est le but de src/native_appearance_resolver.h.

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
  Appearance recovery preserves temporary ability forms, restores the outfit
  on returning to the base form, and reloads resources after reconnects.
  Repeated failures use a cooldown; they no longer disable recovery after
  three repairs for the entire outfit. These changes have offline regression
  coverage; visual verification in a live match is still required.
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
- After a Dota update: usually nothing to do. The DLL re-locates every client.dll
  function and offset it needs at load time, by masked byte signature and by the
  instruction that carries each value, so moved code keeps working without a rebuild.
  Press INSERT: the panel line "Profil client.dll" says whether it resolved, and
  names the entry it could not find if it did not. Only then run
    python refresh_profile.py     (then build.bat)
  It stops rather than record a value that moved, and prints each one with its kind;
  re-run with --accept-changes, or press "Enregistrer quand meme" in Wardrobe's Journal.
  launch_level3.bat runs "refresh_profile.py --check" for you when Python is present.
  That check ignores the addresses a patch moves and warns only on an ABI change.
- Logs in C:\Temp\opencode rotate to *.old once they pass 4 MB.
- The preview overlay uses Game State Integration telemetry. Global cosmetics
  (loading screens, HUD skins, music, terrains, cursor packs, announcers) are in
  the catalog and equip natively; they are client-side in Dota, so they should
  apply in local lobbies, but that path has not been verified in game yet.
- wardrobe_dll.dll = the in-game payload. The application loads it through
  map.exe; never double-click it and never LoadLibrary it. An entry the resolver
  cannot find disables the native appearance profile rather than letting a hook
  land on the wrong function.
- Wardrobe.exe --capture shot.png [--page 0|1|2] saves a picture of the window,
  for a bug report.
- Streamproof is ON: OBS/Discord won't capture the menu.
- The local hero game thread handles appearance updates; the old keeper/TCP writer was removed from the DLL.
