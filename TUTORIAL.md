# Dota 2 Wardrobe — Complete User Guide & Tutorial

Welcome to **Wardrobe**, a modular, high-performance cosmetic loadout manager and model swapper for Dota 2. This guide provides step-by-step instructions for setting up, compiling, and running Wardrobe across all operational levels.

---

## Architecture Overview

Wardrobe contains an external preview tool and an in-game inventory integration:

| Mode | Entry point | Description |
| :--- | :--- | :--- |
| **External preview** | `wardrobe.exe` | Separate cosmetic browser and saved loadout editor using GSI telemetry. |
| **Legacy external swapper** | Existing external tool | Older direct-write implementation, separate from the v6 native appearance path. |
| **Native inventory and appearance v6** | `launch_level3.bat` | Local cosmetic inventory, native Equip handling, and client-side appearance reconstruction for the selected local hero. The DLL no longer uses a TCP or keeper thread to write wearable slots. |

---

## 1. First-Time Setup & Prerequisites

Perform these steps once on your gaming PC:

### 1.1 Software Requirements
1. **Visual Studio 2022 or 2019** (Community edition is free):
   - During VS installation, ensure the **"Desktop development with C++"** workload is checked.
2. **Python 3.8+**:
   - Ensure Python is added to your system `PATH`.
   - Install optional offset analysis dependency (if dumping offsets):
     ```powershell
     pip install pefile
     ```

### 1.2 Copy Dota 2 Game Definition Files
Wardrobe extracts full skin names, rarity, slot data, and item bundle associations directly from Dota 2's game files.

Copy the following two files into your Wardrobe root directory (next to `gen_full_db.py`):
1. **`items_game.txt`**:
   Found in:
   ```
   <Steam>\steamapps\common\dota 2 beta\game\dota\scripts\items\items_game.txt
   ```
2. **`items_english.txt`**:
   Found in:
   ```
   <Steam>\steamapps\common\dota 2 beta\game\dota\resource\localization\items_english.txt
   ```

### 1.3 Generate the Skins Database
Open a terminal in the Wardrobe directory and run:
```powershell
python gen_full_db.py
python gen_names.py
```
- `gen_full_db.py` parses all ~12,000 items, bundles, and heroes, saving the catalog to `data/skins_full.json`.
- `gen_names.py` resolves localized English names for every item token.

### 1.4 Compile Wardrobe
Double-click:
```powershell
build.bat
```
`build.bat` automatically locates your Visual Studio `vcvars64.bat` environment and compiles:
- `build\wardrobe.exe` (External overlay application)
- `build\wardrobe_dll.dll` (In-process swapper payload)
- `build\map.exe` (PE manual-mapping loader)

### 1.5 Install Game State Integration (GSI) Config
Copy [`cfg\gamestate_integration_wardrobe.cfg`](cfg/gamestate_integration_wardrobe.cfg) into your Dota 2 installation:
```
<Steam>\steamapps\common\dota 2 beta\game\dota\cfg\gamestate_integration\
```
*(If the `gamestate_integration` folder does not exist, create it).*

---

## 2. Level 1: External Overlay Mode (Safe & Undetectable)

Level 1 operates purely as an external HUD layer. It has zero process surface on `dota2.exe`.

### How to Run:
1. Start **Dota 2**.
2. Run `build\wardrobe.exe` (no administrator privileges needed).
3. The Wardrobe window will appear over your screen.

### Controls & Features:
- **Everything now lives in `build\Wardrobe.exe`.** Launch it and the home screen states what is ready, what is not, and offers the one action that applies. <kbd>INSERT</kbd> still opens the diagnostic HUD inside the game.
- **Pass-Through Click**: When closed, the window is completely transparent to mouse and keyboard input. You can play your match without interference.
- **Stream-Proof**: Enabled by default (`WDA_EXCLUDEFROMCAPTURE`). Discord screenshare, OBS Studio, and Twitch Studio will not capture or display the overlay.
- **Hero Auto-Detection**: When you enter a match, GSI sends real-time game telemetry over local port 3000. Wardrobe automatically detects your active hero and switches to their loadout page.
- **Hero Search**: Filter heroes on the left column. Names are alphabetically sorted and translated (e.g., `nevermore` shows as `Shadow Fiend`).
- **Slot Management**: Expand any slot (Head, Weapon, Armor, etc.) and click an item to equip it.
- **Full Sets in One Click**: Click `"wear: <Set Name>"` to equip an entire outfit simultaneously.
- **Auto-Save**: Click **"save all"** to record your staged loadout to `loadout.json`. It will reload automatically next time.

---

## 3. Apparence native en partie (v6)

La DLL v6 utilise l'equipement du menu natif de Dota. L'ancien chemin dans
la DLL, qui ecrivait huit slots depuis le navigateur externe via TCP et un
thread de reappliquage, a ete retire. Le navigateur externe reste distinct.
L'ancien mecanisme `offsets.bin` a ete retire ; l'apparence passe uniquement par le profil client.dll verifie.

Les changements confirmes sont regroupes pendant 75 ms, puis le thread du
heros demande une mise a jour native des modeles et effets. La tenue stable
ne provoque pas de reconstruction periodique. Les vues d'objets manquantes
ont au maximum quatre tentatives espacees, puis un etat explicite dans le HUD.

Le profil est lie au SHA-256 du `client.dll` installe, a ses en-tetes PE et aux
entrees des routines. Une version inconnue desactive cette integration.
Voir [le profil et les limites de validation](docs/native-appearance.md).

## 4. Level 3: Local GC inventory receiver

Receiver v6 uses `SteamGameCoordinator001` to process inventory and local Equip requests.
It requests a fresh local cache after attaching, so it also works with an
inventory welcome that arrived before the DLL was loaded. No client.dll address needs to be
configured for the receiver.

### 4.1 Running the updated build

1. Run `build.bat` (or `build.bat --no-pause` from a terminal).
2. Restart Dota if a previous Wardrobe DLL was loaded. Rebuilding the file does
   not replace code already loaded in the game.
3. Wait for Dota's main menu and GC connection, then run `launch_level3.bat`
   or `build\launch_level3.bat` once.
4. Press INSERT. The inventory panel should say **Wardrobe v6**. It attempts a
   cache refresh immediately and retries up to three times, five seconds apart.
5. Inspect the status and the Armory. **Records delivered** means the modified
   bytes were handed to Dota. Confirm the result in the game UI; this status
   alone does not prove Dota accepted or equipped the items.

The launcher needs `map.exe`, `wardrobe_dll.dll`, and `data/skins_full.json`.
The build copies the database into `build/data`, and the loader synchronizes it
into `C:\Temp\opencode` for the in-game DLL. No `offsets.bin` is used by v6.

### 4.2 Diagnosing a failed refresh

The panel shows database count, receive calls, packets received, last message
kind, and refresh attempts. Initialization failures, unsupported packet formats,
and Steam errors show an explicit reason. If no usable inventory arrives within
20 seconds, it stops retrying and displays a timeout. Use **Retry inventory
refresh** after the connection or configuration problem is resolved.

The log is `C:\Temp\opencode\wardrobe_gc.log`. It records initialization,
database source, hook failures, refresh results, packet sizes, and timeouts.
Logs do not contain item ownership credentials or complete packet contents.

**Pause GC receiver** stops refresh attempts and local equip handling. Pending
inventory packets, equip acknowledgements, and outstanding local notifications drain before the hooks are
disabled. After successful refresh, the receiver stays active so native Equip
clicks can update the local loadout. This is a local cache modification and does
not grant Steam ownership or guarantee server acceptance.

### 4.3 Equipping local cosmetics

After refresh, use **Equip** in Dota's native hero/loadout screen. The standalone
`wardrobe.exe` browser still uses its separate model-swap path. The INSERT panel
shows whether local equip is active, accepted and completed equips, pending
replies, and delivery latency. A format error appears there
and in the log instead of silently succeeding.

Receiver v2 only supplied inventory records and then paused; it had no handler
for Equip. Receiver v3 handles single-item (1059) and batched (2569) requests,
updates `CSOEconItem.equipped_state` (field 18), clears the previous item in the
same hero/slot, and returns `SOUpdateMultiple` (26). Batched requests also receive
an `EquipItemsResponse` (2570) addressed to the original client job, using the
same cache version as the update. The current item style is retained unless
the equip request explicitly changes it.

Receiver v4 fixes the delivery gap in v3: a local queue entry alone did not
produce `GCMessageAvailable_t`, so callback-driven clients could wait for the
next real message or click. The receiver now exposes local message notifications
through the existing Steam callback pump (`Steam_BGetCallback` and its paired
`Steam_FreeLastCallback`). It does not switch the game to manual dispatch or
invoke game callbacks on the inventory worker. Genuine Steam callbacks and GC
messages take priority. A local notification is released locally; Steam is
never asked to free a callback it did not allocate. An ignored notification
backs off until a later frame, allowing the dispatch loop to finish.
Each pump emits at most 32 local notifications before yielding, so a burst of
equip requests cannot keep that loop busy indefinitely.

**Accepted / completed** are comparable equip-operation counts. Completed means
both the update and its acknowledgement have been copied to Dota (or the update
alone for legacy single-item requests), not proof that the model rendered.
**Items** counts entries in an outfit; **updates** counts packets, so these two
numbers can differ for a full set. Pending replies and their age expose delayed
delivery. Processing time, notifications, and observed GC state help diagnose
freezes or reconnection without guessing from item counters. Diagnostic file
writes run on the worker, and ordinary GC messages no longer reserve an entire
catalog-sized buffer once the initial inventory has been delivered.

Local item IDs stay within the client. Owned-only equips in untouched slots
still go to Steam. A batch containing local items, or a change to a locally
overridden slot, is handled entirely as a local preview, including any owned
items in that batch. Local selections survive inventory refreshes. Dota can
also save the modified inventory in `cache_<account>_1.soc`, so restarting alone
does not reliably restore the Steam loadout. Real inventory
updates remain visible and are reconciled with the current local selections.

Receiver v6 checks for saved preview items and displays them separately from
records delivered by the current receiver. The HUD includes Dota's PID and the
build timestamp. A process-lifetime marker rejects a second Wardrobe instance
in the same Dota process. It does not install any autostart entry.

The live cache patcher now accepts Dota's econ service (1), including empty
inventories, and leaves service 0 untouched. Previously both services could
receive the full catalog when the first packet contained no econ group. Once
inventory records have been delivered, **Resume inventory receiver** resumes
handling without sending another full-cache refresh or rebuilding all items.

### Restoring Dota's saved inventory cache

Close Dota completely, then run the following from the Wardrobe folder:

```
python repair_inventory_cache.py "<Dota installation>\game\dota" --repair
```

Without `--repair`, this is read-only. Repair recognizes generated IDs together
with their account and known item definitions, verifies a backup, then removes
only affected cache files so Steam can supply a fresh cache on the next game
connection. Clean caches and unknown formats are preserved. Backups and hashes
are kept under `%LOCALAPPDATA%\Wardrobe
ecovery\<timestamp>` (`--list-backups`, `--restore latest`). A visible item in a `.soc` cache
does not prove the DLL is running or that the account owns it.

### 4.4 Packet format and regression tests

The receiver validates message type and routing-header length. It extends
`CMsgSOCacheSubscribed` (24) and caches inside `CMsgClientWelcome` (4004), and
reconciles equipped state in real item updates (21/22/23/26), only when the cache
owner and learned econ service match. Existing item attributes, unrelated cache
objects, metadata, other accounts, and routing headers are retained. Malformed
incoming messages pass through unchanged with a diagnostic; malformed equip
requests are rejected visibly.

`CMsgSOCacheSubscribed.objects` contains `SubscribedType` records, where
`type_id` is field 1 and repeated `object_data` is field 2. It does not contain
`CMsgSOSingleObject` records. `CSOEconItem` uses inventory field 3, quantity field
5, level field 6, and quality field 7; rarity comes from the item definition.

References: [Steam's receive-buffer contract](https://partner.steamgames.com/doc/api/ISteamGameCoordinator),
[Dota GC cache schema](https://github.com/SteamTracking/Protobufs/blob/master/dota2/gcsdk_gcmessages.proto),
[Dota item schema](https://github.com/SteamTracking/Protobufs/blob/master/dota2/base_gcmessages.proto).
Equip formats: [Dota econ messages](https://github.com/SteamDatabase/Protobufs/blob/master/dota2/econ_gcmessages.proto),
[job routing header](https://github.com/SteamDatabase/Protobufs/blob/master/dota2/steammessages.proto).
Delivery: [Steam GC notifications](https://partner.steamgames.com/doc/api/ISteamGameCoordinator#GCMessageAvailable_t),
[Valve callback-dispatch contract](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/steam/steam_api.h).

Run `tests\run_tests.bat` for protocol and simulated-receiver checks. These are
offline checks; the live game's cache acceptance must be verified separately.

---

## 5. Stealth & Security Features Implemented

Wardrobe incorporates multiple layers of security to ensure safety during use:

1. **Direct NT Native Syscalls**:
   - `src/stealth.h` resolves `NtOpenProcess`, `NtReadVirtualMemory`, and `NtWriteVirtualMemory` directly from `ntdll.dll`.
   - Bypasses user-mode hooks and monitoring detours on `kernel32.dll` or `KernelBase.dll`.
2. **Transient Handle Lifecycle**:
   - Process handles are opened on-demand for a few microseconds to write the change and immediately closed with `NtClose`. Wardrobe never maintains an open persistent handle to `dota2.exe`.
3. **Pre-Injection PE Header Wiping**:
   - In `src/loader.cpp`, the manual mapper zeroes out all DOS headers (`MZ`) and NT headers (`PE`) in memory *before* writing the image to the remote process. Security scanners searching for unbacked PE headers find only arbitrary dynamic memory.
4. **Anonymous Window Class & Title**:
   - The external overlay registers under a neutral system class name (`DWM_NotificationOverlay`) with an empty title, preventing window enumeration scans.
5. **Stream-Proof Display Affinity**:
   - `SetWindowDisplayAffinity(..., WDA_EXCLUDEFROMCAPTURE)` is applied to the overlay window, keeping it invisible on Discord, OBS, and Twitch streams.
6. **Isolated GSI Channel**:
   - Game State Integration uses Valve's built-in HTTP POST protocol. It is non-invasive and officially supported by Valve.

---

## 6. Troubleshooting & FAQ

### Q: The overlay does not appear over Dota 2.
- Ensure Dota 2 is running in **"Borderless Windowed"** or **"Windowed"** mode (Settings $\to$ Video $\to$ Display Mode). Exclusive Fullscreen windows block transparent layered desktop overlays.

### Q: Pressing END does not toggle the overlay menu.
- Make sure NumLock is not causing conflicting key states. The menu can also be closed via the **"close overlay"** button in the footer.

### Q: GSI shows "listening on port 3000" but does not detect my hero.
- Verify `gamestate_integration_wardrobe.cfg` is placed in:
  `Steam\steamapps\common\dota 2 beta\game\dota\cfg\gamestate_integration\`
- Restart Dota 2 after copying the configuration file.
- Check that port 3000 is not blocked by another application (e.g. a local web server).

### Q: Dota 2 released a patch. What do I need to redo?
- `python update_db.py` rebuilds `data\skins_full.json` from the new game files, then run `build.bat`.
- In-game appearance usually needs nothing. The DLL re-locates every `client.dll` function and offset
  at load, by masked byte signature and by the instruction that carries each value, so a patch that only
  moves code keeps working. Press <kbd>INSERT</kbd>: the **Profil client.dll** line says whether it
  resolved, and names the entry it could not find otherwise.
- If it does name one, `python refresh_profile.py` re-locates the profile in the new binary using string
  and call-graph anchors and regenerates `src\native_appearance_profile.h`; then run `build.bat`.
  `python refresh_profile.py --check` answers the question without writing anything, and stays quiet
  about the addresses a patch moves; it only complains when a struct offset or a vtable slot changed.
- Regenerating stops rather than record a value that moved, and prints each one with its kind. In the
  Wardrobe window the Journal then offers **Enregistrer quand meme**; from a console, re-run with
  `--accept-changes`.
