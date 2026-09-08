# Dota 2 Wardrobe — Complete User Guide & Tutorial

Welcome to **Wardrobe**, a modular, high-performance cosmetic loadout manager and model swapper for Dota 2. This guide provides step-by-step instructions for setting up, compiling, and running Wardrobe across all operational levels.

---

## Architecture Overview

Wardrobe is structured into three distinct operating tiers, allowing you to choose your desired balance between stealth and features:

| Mode | Risk Level | Description |
| :--- | :--- | :--- |
| **Level 1: External Overlay** | **Zero Risk (Undetectable)** | Runs completely outside the game. Uses Valve's official Game State Integration (GSI) telemetry. Never attaches to, opens, or touches `dota2.exe`. |
| **Level 2A: External Stealth Swapper** | **Minimal Risk** | Runs from `wardrobe.exe`. Directly writes cosmetic definition indices into `dota2.exe` memory using unhooked NT native syscalls (`NtOpenProcess`, `NtWriteVirtualMemory`). Zero DLL injection. |
| **Level 2B: In-Process Manual Map** | **Low/Moderate Risk** | Maps `wardrobe_dll.dll` into `dota2.exe` memory using `map.exe` without `LoadLibrary`. Wipes PE headers prior to mapping. Hooks DirectX 11 Present for an in-game status HUD and runs a keeper thread for respawn persistence. |
| **Level 3: GC Inventory Unlock** | **Smurf / Offline Only** | Injects fake SO cache records directly into client-side game coordinator messages so every cosmetic item appears in Dota's native loadout screen. |

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
Copy [`cfg\gamestate_integration_wardrobe.cfg`](file:///c:/Users/Mahery/Documents/GitHub/rider_register_flutter-main/dota2mods/cfg/gamestate_integration_wardrobe.cfg) into your Dota 2 installation:
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
- **Toggle Visibility**: Press <kbd>INSERT</kbd> on your keyboard to toggle the menu open or closed.
- **Pass-Through Click**: When closed, the window is completely transparent to mouse and keyboard input. You can play your match without interference.
- **Stream-Proof**: Enabled by default (`WDA_EXCLUDEFROMCAPTURE`). Discord screenshare, OBS Studio, and Twitch Studio will not capture or display the overlay.
- **Hero Auto-Detection**: When you enter a match, GSI sends real-time game telemetry over local port 3000. Wardrobe automatically detects your active hero and switches to their loadout page.
- **Hero Search**: Filter heroes on the left column. Names are alphabetically sorted and translated (e.g., `nevermore` shows as `Shadow Fiend`).
- **Slot Management**: Expand any slot (Head, Weapon, Armor, etc.) and click an item to equip it.
- **Full Sets in One Click**: Click `"wear: <Set Name>"` to equip an entire outfit simultaneously.
- **Auto-Save**: Click **"save all"** to record your staged loadout to `loadout.json`. It will reload automatically next time.

---

## 3. Level 2: Live In-Game Model Swap (In-Depth Tutorial)

In Level 2, client-side memory structures in `client.dll` are modified so your hero wears your selected cosmetic items in-game.

Because Dota 2 updates frequently, game memory offsets shift between patches. Wardrobe stores these in a structured binary file named `offsets.bin`.

### 3.1 Understanding the 8 Required Offsets

`offsets.bin` contains 8 specific pointers and offsets in 64-bit `client.dll`:

| Offset Name | Target / Type | Description |
| :--- | :--- | :--- |
| `dwLocalPlayerHero` | `uintptr_t` (RVA) | Pointer from `client.dll` base to the local player's hero entity (`C_DOTA_BaseNPC_Hero*`). |
| `m_hWearables` | `uintptr_t` (Offset) | Offset in `C_DOTA_BaseNPC_Hero` to the array of 8 wearable `EHANDLE`s. |
| `m_ItemView` | `uintptr_t` (Offset) | Offset in `C_EconWearable` pointing to its `CEconItemView` structure. |
| `m_iItemDefIndex` | `uintptr_t` (Offset) | Offset in `CEconItemView` to the 32-bit item definition index (`int32_t`). |
| `m_nFallbackPaint` | `uintptr_t` (Offset) | Offset in `CEconItemView` to the style/paint index (reset to 0 to prevent style glitches). |
| `m_bNeedReapply` | `uintptr_t` (Offset) | Boolean offset in `C_DOTA_BaseNPC_Hero` toggled `true` to trigger model recompilation. |
| `dwEntityList` | `uintptr_t` (RVA) | Pointer from `client.dll` base to the global Source 2 entity list (`VClientEntityList`). |
| `fnFullUpdate` | `uintptr_t` (RVA) | Function pointer in `client.dll` that forces a redraw of the local hero model. |

---

### 3.2 How to Find the Offsets (Reverse Engineering Guide)

Open `<Steam>\steamapps\common\dota 2 beta\game\bin\win64\client.dll` in **IDA Pro**, **Ghidra**, or **x64dbg**.

#### Step 1: Find Strings
1. Press <kbd>Shift</kbd> + <kbd>F12</kbd> in IDA to open the Strings window.
2. Search for:
   - `"C_DOTA_BaseNPC_Hero"`
   - `"C_EconWearable"`
   - `"m_hWearables"`
   - `"m_iItemDefinitionIndex"`

#### Step 2: Extract `m_hWearables` and `dwLocalPlayerHero`
- Follow the cross-reference (xref) from `"m_hWearables"`. You will land in the constructor or netvar binding of `C_DOTA_BaseNPC_Hero`.
- Look for the first 8-element `uint32` array member. The struct offset (typically `0x900` – `0xB00`) is `m_hWearables`.
- Follow xrefs to `GetLocalPlayer` or `C_DOTA_PlayerResource::GetSelectedHeroEntity`. The static RVA pointing to the local hero is `dwLocalPlayerHero`.

#### Step 3: Extract `m_ItemView` and `m_iItemDefIndex`
- Follow the xref to `"C_EconWearable"`.
- Inspect the pointer dereferenced when accessing the item's economy data: this offset is `m_ItemView` (usually `0x2F8` – `0x320`).
- Inside `CEconItemView`, find the field written when reading the definition index: this is `m_iItemDefIndex` (usually `0x180` – `0x240`).

#### Step 4: Extract `dwEntityList`
- Look for string references to `"VClientEntityList003"` in `client.dll`.
- The interface pointer retrieved from the factory is `dwEntityList`.

---

### 3.3 Writing `offsets.bin`

Once you have your 8 hexadecimal offsets, write `offsets.bin`:

```powershell
python dump_offsets.py --write <dwLocalPlayerHero> <m_hWearables> <m_ItemView> <m_iItemDefIndex> <m_nFallbackPaint> <m_bNeedReapply> <dwEntityList> <fnFullUpdate>
```

**Example**:
```powershell
python dump_offsets.py --write 0x3F1A200 0x9B8 0x310 0x228 0x230 0xA40 0x3E80100 0x1A2B3C0
```

This creates a valid 72-byte `offsets.bin`. Copy it into:
- The `build\` directory (next to `wardrobe.exe` and `wardrobe_dll.dll`).
- `C:\Temp\opencode\` (optional backup location).

---

### 3.4 Method 2A: External Stealth Mode (Recommended for Safety)

With `offsets.bin` in your `build\` folder:
1. Start **Dota 2**.
2. Run `build\wardrobe.exe`.
3. Check the **"stealth writer live-push"** checkbox at the bottom of the Wardrobe UI.
4. When you click any skin in the browser:
   - `wardrobe.exe` issues direct NT system calls (`NtOpenProcess`, `NtReadVirtualMemory`, `NtWriteVirtualMemory`).
   - The memory write updates your wearable `ItemDefIndex` and flags `m_bNeedReapply`.
   - Your hero model updates live in-game.
   - **No DLL is injected into Dota 2. No hook is created in Dota's DirectX pipeline.**

---

### 3.5 Method 2B: In-Process Manual Map Mode

If you prefer an in-game status HUD and automatic re-application on death/respawn:

1. Start **Dota 2** and reach the main menu or hero pick phase.
2. In an administrator command prompt, run:
   ```powershell
   build\map.exe build\wardrobe_dll.dll
   ```
   - `map.exe` allocates memory in `dota2.exe`, resolves imports, applies relocations, and wipes all PE headers.
   - It invokes `DllMain` through an execution stub and exits.
3. Start `build\wardrobe.exe`.
4. Equipping skins in `wardrobe.exe` now transmits updates over local loopback (`127.0.0.1:3999`) to the in-process keeper.
5. In-game:
   - Press <kbd>INSERT</kbd> to view the live status overlay rendered directly via DirectX 11 Present.
   - The keeper thread automatically re-applies your chosen loadout whenever your hero respawns.

---

## 4. Level 3: GC Inventory Unlock (Dota 2 Native Armory Unlock)

Level 3 operates at the Steam economy network layer rather than the in-match 3D entity layer.

Instead of swapping models one by one, Level 3 intercepts incoming **Game Coordinator (GC)** network packets from Valve's servers and dynamically injects ~12,000 fake `CSOEconItem` records (generated from `data/skins_full.json`) into your local inventory cache (`CMsgSOCacheSubscribed`).

As a result, **Dota 2's official main-menu Armory, Hero loadout screens, and pre-game pick phases treat all cosmetics as owned by your account.**

> [!CAUTION]
> **Account Safety Notice**: Level 3 tampers with the client-side representation of the Steam economy. While client-side, official Valve ranked servers verify item ownership when loading into a match. If an equipped item is not owned on Steam, the server may reset it or flag the mismatch. **Use Level 3 exclusively for offline practice, bot matches, local lobbies, or on disposable smurf accounts.**

---

### 4.1 How It Works Under the Hood

1. When Dota 2 connects to the Steam network, the Game Coordinator transmits `k_EMsgGCClientWelcome` and `CMsgSOCacheSubscribed`.
2. Inside `client.dll`, a dispatch handler receives this buffer:
   ```protobuf
   message CMsgSOCacheSubscribed {
       repeated CMsgSOSingleObject objects = 2;
   }
   ```
3. In [`src/inventory.cpp`](file:///c:/Users/Mahery/Documents/GitHub/rider_register_flutter-main/dota2mods/src/inventory.cpp), Wardrobe pre-compiles `g_InjectBlob`: a serialized array of `CMsgSOSingleObject` records where each item has `type_id = 1` (econ item) and points to a serialized `CSOEconItem` containing the skin's `def_index`, rarity, and quality.
4. When `hkOnCache` hooks the dispatch function, it appends `g_InjectBlob` to the incoming packet and passes the extended buffer to Dota's original parser.
5. Dota 2 parses your real inventory plus all 12,000 injected skins.

---

### 4.2 Automated 1-Click Method (For Non-Programmers)

You do **not** need IDA Pro, Ghidra, or any reverse engineering experience to use Level 3. Everything has been automated into a single click:

#### Option A: Double-Click `launch_level3.bat` (Easiest)
1. Launch **Dota 2** and wait until you reach the main menu.
2. Double-click **`launch_level3.bat`** (or `build\launch_level3.bat`):
   - Automatically checks if `dota2.exe` is running.
   - If `gc_hook.txt` is missing, it runs `auto_find_gc.py` to detect Steam and find the hook RVA automatically.
   - Automatically syncs the skin database (`skins_full.json`) and offsets.
   - Maps `wardrobe_dll.dll` cleanly into `dota2.exe`.
3. Return to Dota 2, press <kbd>INSERT</kbd>, and navigate to **Heroes $\to$ Armory**. All items will be unlocked!

#### Option B: Zero-Config In-Memory Auto-Scanner
If `gc_hook.txt` is not present, `wardrobe_dll.dll` features a built-in runtime signature scanner (`FindGCHookAuto`). When mapped, it automatically scans `client.dll` memory in real time, resolves the `SOCacheSubscribed` dispatch handler, and hooks it on the fly.

#### Option C: Run `auto_find_gc.py` (Standalone Tool)
If you want to generate `gc_hook.txt` ahead of time without reverse engineering:
```powershell
python auto_find_gc.py
```
- Automatically detects your Steam library folders (even on external drives `D:`, `E:`, etc.).
- Parses `client.dll` with zero external dependencies.
- Discovers the exact RVA and saves `gc_hook.txt` to `build/` and `C:\Temp\opencode\`.

---

### 4.3 Advanced / Manual Method (IDA Pro or Ghidra)

If Dota 2 receives an unusual engine update and you prefer locating the RVA manually:

#### Step-by-step in IDA Pro or Ghidra:
1. Open `<Steam>\steamapps\common\dota 2 beta\game\bin\win64\client.dll` in IDA Pro or Ghidra.
2. Press <kbd>Shift</kbd> + <kbd>F12</kbd> (Strings) and search for:
   ```
   "SOCacheSubscribed"
   ```
3. Double-click the string and press <kbd>X</kbd> to find cross-references (xrefs).
4. The string is passed to an event registration function or used inside `CGCClient::OnSOCacheSubscribed`.
5. Follow the callers up 2 to 3 levels to locate the top-level GC message router (typically `CGCClient::HandleMessage` or `CDOTAGCClient::OnMessage`, which contains a large `switch` statement on message types like `k_EMsgGCClientWelcome`).
6. Identify the function entry point.
7. Calculate the **RVA**:
   $$\text{RVA} = \text{Function Address} - \text{client.dll Image Base}$$
8. Convert this RVA to hexadecimal (e.g., `0x1A2B3C4` $\to$ `1A2B3C4`).
9. Save the hex string into `build\gc_hook.txt`:
   ```powershell
   echo 1A2B3C4 > build\gc_hook.txt
   ```

---

### 4.4 In-Game Verification

1. Press <kbd>INSERT</kbd> to toggle the live status overlay.
2. The HUD will indicate:
   ```
   status: live | inv unlock: 12480 items
   ```
3. Navigate to **Heroes** $\to$ **Armory** in the official Dota 2 menu:
   - All Arcanas, Immortals, taunts, couriers, and personas are unlocked.
   - You can equip items, change styles, and preview animations directly inside the official game interface.

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

### Q: Pressing INSERT does not toggle the menu.
- Make sure NumLock is not causing conflicting key states. The menu can also be closed via the **"close overlay"** button in the footer.

### Q: GSI shows "listening on port 3000" but does not detect my hero.
- Verify `gamestate_integration_wardrobe.cfg` is placed in:
  `Steam\steamapps\common\dota 2 beta\game\dota\cfg\gamestate_integration\`
- Restart Dota 2 after copying the configuration file.
- Check that port 3000 is not blocked by another application (e.g. a local web server).

### Q: Live model swap displays "offsets missing".
- Ensure `offsets.bin` was generated with `python dump_offsets.py --write ...` and placed in `build\` next to `wardrobe.exe`.
- Verify the file size of `offsets.bin` is exactly 72 bytes.

### Q: Dota 2 released a patch and live swap stopped working.
- When Dota 2 receives an update, `client.dll` changes. Run `python dump_offsets.py` against the new `client.dll`, update the 8 offsets, and generate a new `offsets.bin`.
