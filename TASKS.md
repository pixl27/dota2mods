# Wardrobe — work ledger

## 2026-10-02 fixes (done, in-game check pending)
- [x] Profile refresh, hero baseline table, animation translation, persona dragon swap, item modifiers
- [ ] In-game check by the user: Razor arcana animations, DK persona dragon form, log anims=/forms=

## 2026-10-07 Dota update
- [x] DestroyWearables lost its signature (frame size 0x50 -> 0x40); C_DOTA_BaseNPC grew 0xF0-0x100
- [x] Signatures mask frame sizes; DestroyWearables has a shape anchor; 15 offsets cross-checked against field tables
- [x] wardrobe_tools.exe (C++): `compat` (DLL resolver vs installed client.dll) and `repair` (port of repair_inventory_cache.py, same counts on 13 real caches, Python backups restore)
- [x] Home check "Compatible avec cette version de Dota 2", incompatible state, "Mettre à jour Wardrobe" (refresh + rebuild while the app is open: verified, exe moved aside)
- [x] Package ships wardrobe_tools.exe; README says Python is not needed

## One-click installer for a friend without Python
- [x] setup.cpp: one window, one button; payload appended to the exe (LZMS + SHA-256, Windows APIs only)
- [x] Per-user install to %LOCALAPPDATA%\Programs\Wardrobe, asInvoker manifest (no admin); Desktop + Start menu shortcuts; launches Wardrobe
- [x] Running it again updates (locked exe moved aside, tested); uninstaller registered in Windows Settings (keeps inventory backups)
- [x] package.py builds Wardrobe-Setup-<date>.exe (2.7 MB); import check covers it; file dates preserved
- [x] tests/test_installer.py: install identical files, update while locked, uninstall, refuse foreign folder, refuse damaged payload (3 positions), stub alone
- [ ] Real install with shortcuts + registry and uninstall from Windows Settings: not run here (would touch the user's Desktop/registry)

## UI rework ("ultra beautiful", app + installer, one visual system)
Direction: a dark, deep, quiet surface with one luminous accent; depth from light, not from boxes.
- Aim for: near-black blue-tinted background with a soft radial glow; few large surfaces with hairline
  borders and a faint top highlight; Bahnschrift display titles, Segoe UI body with clear hierarchy;
  real icons (Segoe Fluent Icons / MDL2); one gradient accent (violet -> azure) for the single primary
  action; status colours (green, amber, red) used only for state; motion that explains (page fade/slide,
  hover lift, pressed state, progress easing, a breathing status orb), never decoration.
- Avoid: default ImGui widgets and grey slabs; every block in an identical card ("card soup"); walls of
  13 px grey text; more than one primary button per screen; status colours on clickable things;
  rainbow gradients, neon outlines, drop shadows on everything; Valve/Dota logos or branding.
- [x] App icon (assets/make_icon.py -> wardrobe.ico + brand_mark.h), embedded in Wardrobe.exe and the installer
- [x] theme.h v2: palette, fonts (Bahnschrift display, Segoe Fluent/MDL2 icons), backdrop, glows, panels, buttons, orb, tiles, nav, progress, page transition
- [x] Rail, Accueil, Entretien, Journal reworked; captures reviewed (ready, incompatible x2, Entretien, Journal empty/filled)
- [x] Installer UI in the same system; captures reviewed (ready, update, working, done, failed, remove, removed, broken)
- [x] Build (0 warnings), all tests, package + installer; installer verified in scratch folders
- [ ] Not seen: Windows 10 rendering (icon font falls back to MDL2), the restore confirmation dialog, hover/press animations (captures are still frames)

## Self-service for a friend without Python (2026-10-07)
- [x] Catalog rebuilt in C++ (`wardrobe_tools catalog-build`, src/catalog_builder.h): identical bytes to gen_full_db.py + gen_names.py (tests/test_catalog_builder.py)
- [x] `wardrobe_tools catalog`: counts cosmetics of the installed Dota missing from the catalog (0 now; 116 with the 09/09 catalog)
- [x] Home: "De nouveaux cosmétiques sont sortis" notice + catalog tile; clicked for real in a test install: catalog rebuilt in 1 s, notice gone
- [x] Auto-update: src/app/update.h (GitHub latest release, WinHTTP), installer --update; clicked for real against a local fake release: downloaded, installed without a click, reopened
- [x] release.py: version, build, tests, package, and (only with --publish) GitHub release + asset; dry run OK in 56 s
- [x] User's own install (%LOCALAPPDATA%\Programs\Wardrobe) replaced by 2026.10.07.1 (new catalog, 0 missing)
- [x] First public release v2026.10.07.1 published (https://github.com/pixl27/dota2mods/releases/tag/v2026.10.07.1); public download identical to the tested installer; update check and redirect download verified against the real GitHub
- [ ] Friend: one manual install from the release page (earlier installers have no updater); automatic afterwards
