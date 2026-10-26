# s2fix: performance fixes for Total War: Shogun 2

Runtime fixes that make battles in Total War: Shogun 2 render faster. There is one package for each version of the game:
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

Every fix is on by default and `s2prof.ini` comes preconfigured. There is nothing to set up.

To uninstall, delete the proxy DLL, `s2prof.dll`, `s2prof.ini`, the `fixes\` folder and the `perf-logs\` folder.

## In game

You don't need any hotkeys to play. They exist to compare the game with the fixes off and on.

| Hotkey | Does |
|---|---|
| Ctrl+Shift+F10 | switches all fixes off and on (on at start) |
| Ctrl+Shift+F9 | 20-second A/B capture: all fixes switch off and on every 2 s; the summary goes to the log |
| Ctrl+Shift+F8 | 20-second capture without switching anything |

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

## Shogun 2 — Complete Edition

### Requirements

- Windows 10 or 11, 64-bit.
- The engine build these fixes were made for: `empire.retail.x64.dll` with PE timestamp `0x6ac7c293` (SHA-256 starting
  `f1b609dffb95c1fa`). Each fix checks the engine code it changes first. On any other build it stays off and the log says
  why.
- Tested on native D3D11 with an NVIDIA driver.

### Files

```
d3d11.dll        loads s2prof.dll and passes everything else to Windows' own d3d11.dll
s2prof.dll       loads the fixes and provides the hotkeys and the log
s2prof.ini       settings
fixes\           one DLL per fix
```

### Settings (`s2prof.ini`)

| Key | Default | Meaning |
|---|---|---|
| `[proxy] multithreaded_device` | 1 | at start: creates the D3D11 device without the single-thread flag; the overhaul's worker threads need it |
| `[proxy] wrap_context` | 1 | at start: gives the game forwarding wrappers of the device and context |
| `[cpu] ccd` | cache | keeps the game's threads on one CCD: `cache` picks the CCD with the largest L3 (the 3D V-Cache CCD of a dual-CCD X3D chip) and does nothing on other CPUs; `off` leaves it to Windows; a number picks that CCD |
| `[s2prof] console` | 0 | 1 opens a window that shows the log live |

### The fixes

| Fix | What it does |
|---|---|
| `overhaul` | spreads the draw submission of the heavy views over worker threads (D3D11 deferred contexts, replayed in order) |
| `batchsplit` | splits large render-item batches into ordered chunks on helper threads |
| `walksplit` | runs the draw-time batching walk's filtering and LOD picks on helper threads |
| `entrysort` | replaces a per-entry sort whose comparator recomputed distances on every comparison |
| `tiegroup` | sorts tied render items so instanced draws group properly |
| `extrasrecord` | records per-object extras in bulk instead of one call per index |
| `listflag` | stops the parallel render-list append from rewriting a shared flag on every call |
| `instancecopy` | copies per-draw instance records with 16-byte moves instead of a memcpy call each |
| `itemcopy` | copies 176-byte render items whole |
| `imposterreuse` | reuses imposter uploads already in the geometry buffer this frame |
| `texturecache` | caches the HUD's per-quad texture-name lookups |
| `uiprefetch` | prefetches the UI tree's next nodes during its update |
| `powcache` | caches gamma-to-linear colour conversions (`powf(x, 2.2)`) |
| `luagc` | steps the shared Lua heap's collector once per frame instead of once per Lua state |
| `focuscache` | asks Windows for the foreground window once per frame instead of on every key query |
| `scenelock` | spins longer on the unit emission lock instead of sleeping in the kernel |
| `cpuplace` | keeps the game's threads on the CCD `[cpu] ccd` names |

`s2prof.dll` also replaces two engine sorts (render items and effect objects) with stable radix sorts.

## Shogun 2, the original game

### Requirements

- Windows 10 or 11, 64-bit.
- The Steam version with the May 2023 update: `empire.retail.dll` with PE timestamp `0x645e92bb` (SHA-256 starting
  `022fae2e2da92b59`). Each fix checks the engine code it changes first. On any other build the fixes stay off and the
  log says why.
- DirectX 11 mode. The fixes support only DirectX 11: in DirectX 9 mode they stop the game with a message.
- Tested with an NVIDIA driver.

### Files

```
dinput8.dll      loads s2prof.dll and passes everything else to Windows' own dinput8.dll
s2prof.dll       loads the fixes and provides the hotkeys and the log
s2prof.ini       settings
fixes\           one DLL per fix
```

The game loads `dinput8.dll` in both DirectX modes, before it creates its device. That is how the fixes can refuse
DirectX 9 mode.

### Settings (`s2prof.ini`)

| Key | Default | Meaning |
|---|---|---|
| `[device] multithreaded_device` | 1 | at start: creates the D3D11 device without the single-thread flag; the overhaul's worker threads need it |
| `[cpu] ccd` | cache | keeps the game's threads on one CCD: `cache` picks the CCD with the largest L3 (the 3D V-Cache CCD of a dual-CCD X3D chip) and does nothing on other CPUs; `off` leaves it to Windows; a number picks that CCD |
| `[flipmodel] enabled` | 1 | at start: creates the swap chain with the flip model instead of the 2011 blt model; 0 keeps the game's |
| `[flipmodel] buffers` | 3 | at start: flip-model back buffers, 2 to 4 |
| `[flipmodel] tearing` | 1 | at start: allows tearing (an uncapped frame rate, variable refresh in a window) when Windows supports it |
| `[s2prof] console` | 0 | 1 opens a window that shows the log live |

### The fixes

| Fix | What it does |
|---|---|
| `overhaul` | spreads the draw submission of the shadow cascades and the G-buffer view over worker threads (D3D11 deferred contexts, replayed in order) |
| `keysplit` | computes the unit render lists' keys on helper threads while the main thread fills the instance textures |
| `batchsplit` | splits the soldier render batch into ordered chunks on helper threads |
| `walksplit` | runs the draw-time batching walk's filtering and LOD picks on helper threads |
| `itemsort` | replaces the three render-item sorts with stable sorts that leave an already sorted list alone |
| `itemcopy` | copies 312-byte render items whole |
| `listkeep` | keeps the render lists' per-thread buffers between frames instead of regrowing them every frame |
| `listflag` | stops the parallel render-list append from rewriting a shared flag on every call |
| `instancecopy` | copies per-draw instance records with 16-byte moves instead of a memcpy call each |
| `extrasrecord` | records per-object extras in bulk instead of one call per index |
| `imposterreuse` | reuses imposter uploads already in the geometry buffer this frame |
| `portraitprep` | for the general's portrait, prepares only the portrait's own items and texture rows, not the whole battle again |
| `buildingblock` | skips a loop in the building draw whose results are never used |
| `entrysort` | replaces a per-entry sort whose comparator recomputed distances on every comparison |
| `texturecache` | caches the HUD's per-quad texture-name lookups |
| `uiprefetch` | prefetches the UI tree's next nodes during its update |
| `powcache` | caches gamma-to-linear colour conversions (`powf(x, 2.2)`) |
| `luagc` | steps the shared Lua heap's collector once per frame instead of once per Lua state |
| `focuscache` | asks Windows for the foreground window once per frame instead of on every key query |
| `scenelock` | spins longer on the unit emission lock instead of sleeping in the kernel |
| `flipmodel` | creates the swap chain with the flip model, which avoids the 2011 blt model's Present stalls |
| `cpuplace` | keeps the game's threads on the CCD `[cpu] ccd` names |
| `maincore` | keeps the game's other threads off the main thread's CPU core, so the main thread has the whole core |

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
                 (VERSION=v0.2.0 names them; a local build gets a timestamp)
```

Each game's folder also has its own `build.sh` and `package.sh`. The sort tests are in `shogun2ce/tests/` and
`shogun2og/tests/`; `.github/workflows/release.yml` shows how to build and run them.

## License

GNU General Public License v3.0; see `LICENSE`.
