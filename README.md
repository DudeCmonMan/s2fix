# s2fix: performance fixes for Total War: Shogun 2

Runtime fixes that make battles in Total War: Shogun 2 render faster, and its campaign map and end turns run faster.
There is one package for each version of the game:
the Complete Edition and the original 2011 game. No game file is changed: a proxy DLL next to the game loads the fixes,
and they patch the engine in memory only.

This is an unofficial project, not affiliated with or endorsed by SEGA or Creative Assembly.

## Which package

| Game | Package | Unzip next to |
|---|---|---|
| Shogun 2 — Complete Edition (64-bit) | `s2fix_shogun2_complete-edition_<version>.zip` | `shogun2_complete_edition.exe` |
| Shogun 2, the original game (32-bit) | `s2fix_shogun2_original_<version>.zip` | `Shogun2.exe` |

## Install

1. Unzip the package into the game folder (see the table above). It adds a proxy DLL, `s2prof.dll`, `s2prof.ini` and a
   `fixes\` folder.
2. Start the game as usual.

Every fix is on by default and `s2prof.ini` comes preconfigured. There is nothing to set up. The one opt-in is the
original game's borderless windowed mode (`[window] borderless=1`, see its settings).

To uninstall, delete the proxy DLL, `s2prof.dll`, `s2prof.ini`, the `fixes\` folder and the `perf-logs\` folder.

## In game

You don't need any hotkeys to play. Most exist to compare the game with the fixes off and on; Ctrl+Shift+F5 is for a
picture gone wrong.

| Hotkey | Does |
|---|---|
| Ctrl+Shift+F10 | switches all fixes off and on (on at start) |
| Ctrl+Shift+F9 | 20-second A/B capture: all fixes switch off and on every 2 s; the summary goes to the log |
| Ctrl+Shift+F8 | 20-second capture without switching anything |
| Ctrl+Shift+F6 | debug report (see "Reporting a problem" below) |
| Ctrl+Shift+F5 | a snapshot, then the game rebuilds its graphics (see "When the picture goes wrong" below) |

A few settings apply when the game starts (marked "at start" below). The hotkeys don't switch those.

### Comparing fps

1. Turn on an fps counter, for example the Steam overlay's.
2. In a busy battle, keep the camera still. Fps depends heavily on what is on screen.
3. Press Ctrl+Shift+F10 a few times and compare the fps with the fixes off and on.

The fps in the capture summaries is an average over the whole capture, so camera movement, loading and stutters all
blend into one number. Use the captures as diagnostics for a bug report, not as a benchmark.

### The log

Each run writes `perf-logs\<date-time>_s2prof.log` in the game folder. It lists which fixes installed, the hotkeys used,
capture results and, if the game crashes, where. Captures also write thread and stack files there.

### Reporting a problem

1. In a battle, put the camera somewhere busy and keep it still.
2. Press Ctrl+Shift+F6. You hear a beep, and a banner in the middle of the screen shows "Snapshot Running", what it is
   testing and how far along it is. Don't touch the camera until the banner goes away and you hear the second beep,
   about 3 minutes later.
3. Attach `perf-logs\report_<date-time>.zip` from the game folder to your issue.

The report records your system (CPU, memory, GPUs and drivers, displays), the game's settings, the fixes and how they
installed, every program module loaded into the game, its threads, and timings with all the fixes on and off and then
each fix on and off. It also takes screenshots of the game (only the game's own picture, never your desktop) with the
fixes on and off. While it works, files go to `perf-logs\temp`; it packs them into the zip and removes them. Your user
name and computer name are replaced in everything it writes. Nothing is sent anywhere: you decide what to share.

### When the picture goes wrong

If textures, shadows or other graphics look broken and stay that way, press Ctrl+Shift+F5. s2fix first saves
`perf-logs\snapshot_<date-time>.zip`: a screenshot of what the game showed, the state of the graphics card as the game
sees it, and the display driver's recent events, with the same system details and name replacement as the debug
report. Then the game releases and rebuilds all of its graphics, the way it recovers when the graphics card drops out:
the loading screen shows for a moment (longer with the fixes off) and the game carries on. If the problem comes back,
attach the snapshot to your issue.

Resets are at least 5 seconds apart: a press sooner does nothing, and the log says why. On the Complete Edition, after
several resets in a row, shadows can flicker as the camera moves over an area.

## Shogun 2 — Complete Edition

### Requirements

- Windows 10 or 11, 64-bit.
- The engine build these fixes were made for: `empire.retail.x64.dll` with PE timestamp `0x6ac7c293` (SHA-256 starting
  `f1b609dffb95c1fa`). If `empire.retail.x64.dll` is a different build, or another mod's copy of it, a warning asks
  whether to launch. Either way, each fix checks the engine code it depends on and stays off if it was changed; the log
  lists which.
- Tested on native D3D11 with an NVIDIA driver.

### Files

```
d3d11.dll        loads s2prof.dll and passes everything else to Windows' own d3d11.dll
s2prof.dll       loads the fixes and provides the hotkeys and the log
s2prof.ini       settings
fixes\           one DLL per fix
```

The game loads `d3d11.dll` as it starts its renderer. It loads `s2prof.dll`, which holds the game's device creation
until the fixes are installed. If s2fix is loaded later (injected while the game runs), a message says that some fixes
are off for that session.

### Settings (`s2prof.ini`)

| Key | Default | Meaning |
|---|---|---|
| `[device] multithreaded_device` | 1 | at start: creates the D3D11 device without the single-thread flag; the overhaul's worker threads need it |
| `[device] wrap_context` | 1 | at start: gives the game forwarding wrappers of the device and context |
| `[cpu] ccd` | auto | which CPUs the game's threads use: `auto` gives the main thread the best core of the CPU's largest cache group (the 3D V-Cache CCD of an X3D chip, the P-cores of an Intel chip) and keeps the other threads on that group when it leaves them 10 CPUs, else on every CPU; a number picks a cache group (0 holds CPU 0); `off` leaves the other threads to Windows, and the main thread still gets its core. `cache`, the old default, means `auto` |
| `[s2prof] console` | 0 | 1 opens a window that shows the log live |
| `[s2prof] ignore_build_check` | 0 | 1 skips the warning shown when `empire.retail.x64.dll` is a different build or another mod's copy of it; each fix still checks the engine code it depends on |

### The fixes

| Fix | What it does |
|---|---|
| `overhaul` | spreads the draw submission of the heavy views over worker threads (D3D11 deferred contexts, replayed in order) |
| `batchsplit` | splits large render-item batches into ordered chunks on helper threads |
| `walksplit` | runs the draw-time batching walk's filtering and LOD picks on helper threads |
| `contention` | stops idle helper threads from crowding out the overhaul's workers when the game's threads share one CCD: the workers run above normal priority, and the helpers spin only briefly before sleeping |
| `entrysort` | replaces a per-entry sort whose comparator recomputed distances on every comparison |
| `tiegroup` | sorts tied render items so instanced draws group properly |
| `extrasrecord` | records per-object extras in bulk instead of one call per index |
| `listflag` | stops the parallel render-list append from rewriting a shared flag on every call |
| `instancecopy` | copies per-draw instance records with 16-byte moves instead of a memcpy call each |
| `itemcopy` | copies 176-byte render items whole |
| `listkeep` | keeps the render lists' per-thread buffers between frames instead of regrowing them every frame |
| `imposterreuse` | reuses imposter uploads already in the geometry buffer this frame |
| `texturecache` | caches the HUD's per-quad texture-name lookups |
| `uiprefetch` | prefetches the UI tree's next nodes during its update |
| `powcache` | caches gamma-to-linear colour conversions (`powf(x, 2.2)`) |
| `luagc` | steps the shared Lua heap's collector once per frame instead of once per Lua state |
| `focuscache` | asks Windows for the foreground window once per frame instead of on every key query |
| `scenelock` | spins longer on the unit emission lock instead of sleeping in the kernel |
| `buildingblock` | skips work the building draw throws away every frame: a loop over each building piece's neighbours, and the value that decided it (most noticeable in sieges and towns) |
| `cpuplace` | keeps the game's other threads on the main thread's CCD or P-cores, as `[cpu] ccd` sets |
| `maincore` | gives the main thread the best CPU core and keeps the game's other threads off it |

On the campaign map:

| Fix | What it does |
|---|---|
| `camprelations` | remembers where each faction's record is: every character on the map searched the whole faction list for it several times a frame |
| `campappearance` | remembers which appearance entry each character's model uses instead of searching the character's appearance set again every frame |
| `cailookup` | in the end turn, remembers where the AI finds each faction's value for another faction: rating how two factions stand, it averages over every other faction and searched two faction tables for each, so the end turn's cost grew with the cube of the faction count (the result is the same) |
| `textequal` | in the end turn, compares the AI's names with fixed text in place, instead of copying the text through the engine's locked memory pool for every comparison (the answer is the same) |

When loading:

| Fix | What it does |
|---|---|
| `packcache` | keeps the game's pack files in Windows' file cache between starts: the game reopened them in a way that bypasses the cache, so every start read all its data from disk again |
| `packindex` | gives each pack's list of files its full size from the start: the game started every list small and enlarged it only once it was completely full, so the big packs took longer and longer to list (the files found are the same) |
| `lookupcache` | remembers, for the rest of the session, each loose file in `data\` or a mod's folder that Windows said is not there, so the game's repeated checks for it, in loads and end turns, are answered from memory; loose files that exist (mods) are always found |
| `pathsplit` | builds a battle's pathfinding map on several threads: its searches over the map's triangles are split across helper threads, and the game's second and third tries run alongside the first (the result is the same) |

`s2prof.dll` also replaces two engine sorts (render items and effect objects) with stable sorts: a counting sort when a list has few distinct keys, otherwise a radix sort.

## Shogun 2, the original game

### Requirements

- Windows 10 or 11, 64-bit.
- The Steam version with the May 2023 update: `empire.retail.dll` with PE timestamp `0x645e92bb` (SHA-256 starting
  `022fae2e2da92b59`). If `empire.retail.dll` is a different build, or another mod's copy of it, a warning asks whether
  to launch. Either way, each fix checks the engine code it depends on and stays off if it was changed; the log lists
  which.
- DirectX 11 mode. The fixes are for DirectX 11 only: in DirectX 9 mode they don't load, and the game runs unchanged.
  If the game falls back to DirectX 9 on its own, a message says so and the fixes stay off.
- Tested with an NVIDIA driver.

### Files

```
d3d11.dll        loads s2prof.dll and passes everything else to Windows' own d3d11.dll
s2prof.dll       loads the fixes and provides the hotkeys and the log
s2prof.ini       settings
fixes\           one DLL per fix
```

The game loads `d3d11.dll` only in DirectX 11 mode, as it creates its device. It loads `s2prof.dll`, which holds the
game's device creation until the fixes are installed. If s2fix is loaded later (injected while the game runs), a
message says that some fixes are off for that session.

### Borderless windowed

The game itself only has a framed window or exclusive fullscreen. s2fix can run it borderless instead, covering the
whole monitor. The game renders at the resolution set in its options and is scaled to fill the screen, and a
resolution change applies at once. A resolution with another aspect ratio than the monitor's is stretched:

1. In the game's options, choose windowed mode.
2. In `s2prof.ini`, set `borderless=1` under `[window]`.
3. Restart the game.

The cursor stays on the game's monitor while the game has focus, so edge scrolling works with several monitors.
Alt+Tab releases it. To let it move freely, set `clip_cursor=0`.

### Settings (`s2prof.ini`)

| Key | Default | Meaning |
|---|---|---|
| `[device] multithreaded_device` | 1 | at start: creates the D3D11 device without the single-thread flag; the overhaul's worker threads need it |
| `[cpu] ccd` | auto | which CPUs the game's threads use: `auto` gives the main thread the best core of the CPU's largest cache group (the 3D V-Cache CCD of an X3D chip, the P-cores of an Intel chip) and keeps the other threads on that group when it leaves them 10 CPUs, else on every CPU; a number picks a cache group (0 holds CPU 0); `off` leaves the other threads to Windows, and the main thread still gets its core. `cache`, the old default, means `auto` |
| `[flipmodel] enabled` | 1 | at start: creates the swap chain with the flip model instead of the 2011 blt model; 0 keeps the game's |
| `[flipmodel] buffers` | 3 | at start: flip-model back buffers, 2 to 4 |
| `[flipmodel] tearing` | 1 | at start: allows tearing (an uncapped frame rate, variable refresh in a window) when Windows supports it |
| `[flipmodel] latency` | 2 | at start: at most this many frames queued, waiting for room after each Present instead of blocking inside it; 0 keeps Windows' 3 |
| `[window] borderless` | 0 | at start: 1 runs the game's windowed mode in a borderless window that covers its monitor, at the resolution set in the game's options, scaled to fit; set the game to windowed, not fullscreen |
| `[window] clip_cursor` | 1 | with borderless: keeps the cursor on the game's monitor while the game has focus, so edge scrolling works with several monitors |
| `[vram] enabled` | 1 | at start: gives the game the graphics card's video memory, up to the 4095 MB it can count, instead of the 2.9 GB a 32-bit game is told; 0 keeps the game's figure |
| `[s2prof] console` | 0 | 1 opens a window that shows the log live |
| `[s2prof] ignore_build_check` | 0 | 1 skips the warning shown when `empire.retail.dll` is a different build or another mod's copy of it; each fix still checks the engine code it depends on |

### The fixes

| Fix | What it does |
|---|---|
| `overhaul` | spreads the draw submission of the shadow cascades and the G-buffer view over worker threads (D3D11 deferred contexts, replayed in order) |
| `keysplit` | computes the unit render lists' keys on helper threads while the main thread fills the instance textures |
| `batchsplit` | splits the soldier render batch into ordered chunks on helper threads |
| `walksplit` | runs the draw-time batching walk's filtering and LOD picks on helper threads |
| `contention` | stops idle helper threads from crowding out the overhaul's workers when the game's threads share one CCD: the workers run above normal priority, and the helpers spin only briefly before sleeping (fewer stutters in heavy scenes such as sieges) |
| `itemsort` | replaces the three render-item sorts with stable sorts that leave an already sorted list alone |
| `itemcopy` | copies 312-byte render items whole |
| `listkeep` | keeps the render lists' per-thread buffers between frames instead of regrowing them every frame |
| `listflag` | stops the parallel render-list append from rewriting a shared flag on every call |
| `instancecopy` | copies per-draw instance records with 16-byte moves instead of a memcpy call each |
| `extrasrecord` | records per-object extras in bulk instead of one call per index |
| `imposterreuse` | reuses imposter uploads already in the geometry buffer this frame |
| `portraitprep` | for the general's portrait, prepares only the portrait's own items and texture rows, not the whole battle again |
| `buildingblock` | skips work the building draw throws away every frame: a loop over each building piece's neighbours, and the value that decided it (most noticeable in sieges and towns) |
| `entrysort` | replaces a per-entry sort whose comparator recomputed distances on every comparison |
| `texturecache` | caches the HUD's per-quad texture-name lookups |
| `uiprefetch` | prefetches the UI tree's next nodes during its update |
| `powcache` | caches gamma-to-linear colour conversions (`powf(x, 2.2)`) |
| `luagc` | steps the shared Lua heap's collector once per frame instead of once per Lua state |
| `focuscache` | asks Windows for the foreground window once per frame instead of on every key query |
| `scenelock` | spins longer on the unit emission lock instead of sleeping in the kernel |
| `flipmodel` | replaces the game's 2011 swap chain, which copies every frame through the desktop compositor and stalls now and then, with a flip-model one: no stalls, tearing allowed (an uncapped frame rate, variable refresh in a window), and at most two frames queued for lower input lag |
| `vram` | By default, Windows tells Shogun 2 to use a maximum of 3GB (which Shogun 2 uses 95% of), this patch pushes it to as much as your GPU can give (up to 4GB, engine limitation). |
| `borderless` | with `[window] borderless=1`: borderless windowed mode at the resolution set in the game's options, scaled to the monitor, with the mouse scaled to match (the game itself has only a framed window or exclusive fullscreen) |
| `cpuplace` | keeps the game's other threads on the main thread's CCD or P-cores, as `[cpu] ccd` sets |
| `maincore` | gives the main thread the best CPU core and keeps the game's other threads off it |
| `timerperiod` | holds Windows' 1 ms timer period, which the game otherwise switches on and off around every timestamp |

On the campaign map:

| Fix | What it does |
|---|---|
| `camprelations` | remembers where each faction's record is: every character on the map searched the whole faction list for it several times a frame |
| `campappearance` | remembers which appearance entry each character's model uses instead of searching the character's appearance set again every frame |
| `cailookup` | in the end turn, remembers where the AI finds each faction's value for another faction: rating how two factions stand, it averages over every other faction and searched two faction tables for each, so the end turn's cost grew with the cube of the faction count (the result is the same) |
| `textequal` | in the end turn, compares the AI's names with fixed text in place, instead of copying the text through the engine's locked memory pool for every comparison (the answer is the same) |

When loading:

| Fix | What it does |
|---|---|
| `texdirect` | creates the game's compressed textures straight from their files instead of having D3DX decompress and recompress every block, which took most of a load; the textures show the artwork's own blocks |
| `packcache` | keeps the game's pack files and loose data files in Windows' file cache between starts: the game opened them in a way that bypasses the cache, so every start read all its data from disk again |
| `packindex` | gives each pack's list of files its full size from the start: the game started every list small and enlarged it only once it was completely full, so the big packs took longer and longer to list (the files found are the same) |
| `randseed` | starts the game's 2009 network encryption library without its slowest step: to gather randomness it read through every block of memory the game had allocated, twice during start-up, which took seconds and longer with mods; it keeps every other source, Windows' own random generator included |
| `soundprobe` | lets the sound system test which speaker layouts work on a helper thread while the game goes on loading, instead of the game waiting for twelve audio device tests at start-up; the game still opens the audio device itself, as before |
| `lookupcache` | remembers, for the rest of the session, each loose file in `data\` or a mod's folder that Windows said is not there, so the game's repeated checks for it, in loads and end turns, are answered from memory; loose files that exist (mods) are always found |
| `pathsplit` | builds a battle's pathfinding map on several threads: its searches over the map's triangles are split across helper threads, and the game's second and third tries run alongside the first (the result is the same) |

## Privacy and safety

The fixes make no network connections and have no remote access. They read `s2prof.ini` and the game's own memory, and
write only the `perf-logs` folder.

## Building

Releases are built by GitHub Actions from the tagged source (`.github/workflows/release.yml`), with WinLibs GCC 16.2.0
(POSIX threads, MSVCRT, r2) checked against its published SHA-256. The build log prints each zip's SHA-256.

To build yourself, put both MinGW-w64 compilers on the path (`x86_64-w64-mingw32-gcc` for the Complete Edition,
`i686-w64-mingw32-gcc` for the original game) and use a POSIX shell (Git Bash works):

```
./build.sh       builds both games: shogun2ce/ and shogun2og/
./package.sh     builds, then writes dist/s2fix_shogun2_complete-edition_<version>.zip and
                 dist/s2fix_shogun2_original_<version>.zip
                 (VERSION=v0.2.1 names them; a local build gets a timestamp)
```

Each game's folder also has its own `build.sh` and `package.sh`. The sort tests are in `shogun2ce/tests/` and
`shogun2og/tests/`; `.github/workflows/release.yml` shows how to build and run them.

## License

GNU General Public License v3.0; see `LICENSE`.
