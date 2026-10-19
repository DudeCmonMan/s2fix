# s2fix: performance fixes for Total War: Shogun 2 — Complete Edition

Runtime fixes for the Complete Edition's engine (`empire.retail.x64.dll`) that make battles render faster. The biggest
is the **render overhaul**, which spreads the engine's draw submission over several threads with D3D11 deferred
contexts, so the GPU is no longer kept waiting on a single CPU thread. None of the game's own files are changed: the
fixes load through a `d3d11.dll` placed next to the game and patch the engine in memory only.

This is an unofficial project, not affiliated with or endorsed by SEGA or Creative Assembly.

On the test machine, in a heavy battle with many effects, the overhaul took the game from **76.8 to 107.5 fps** and the
GPU from 50% to 71% busy (same session, switched on and off every 2 seconds).

## Requirements

- Windows 10 or 11, 64-bit.
- Total War: Shogun 2 — Complete Edition with the engine build these fixes were made for: `empire.retail.x64.dll` with
  PE timestamp `0x6ac7c293` (SHA-256 starting `f1b609dffb95c1fa`). Each fix checks the engine code it changes before
  changing it. On any other build it stays out and the log says why; a game update needs updated fixes.
- Tested on native D3D11 with an NVIDIA driver.

## Install

Unzip the package into the game folder, next to `shogun2_complete_edition.exe`:

```
d3d11.dll        loads s2prof.dll and passes everything else to Windows' own d3d11.dll
s2prof.dll       loads the fixes and provides the hotkeys and the log
s2prof.ini       settings
fixes\           one DLL per fix
```

Start the game as usual. **Every fix is on by default and `s2prof.ini` comes preconfigured, so there is nothing to set
up or switch on.** To uninstall, delete those four entries and the `perf-logs` folder.

## In game

You don't need any hotkeys to play. They exist to compare the game with the fixes off and on:

| Hotkey | Does |
|---|---|
| Ctrl+Shift+F7 | switch the render overhaul off and on (on at start) |
| Ctrl+Shift+F10 | switch the other fixes off and on (on at start) |
| Ctrl+Shift+F9 | 20-second A/B capture: the other fixes alternate every 2 s, the summary goes to the log |
| Ctrl+Shift+F8 | 20-second capture without switching anything |

### Seeing the difference

The most reliable test is to switch the fixes off and on live:

1. Turn on an fps counter (the Steam overlay's, for example).
2. In a busy battle, keep the camera still: fps depends heavily on what is on screen.
3. Press Ctrl+Shift+F7 (the overhaul) or Ctrl+Shift+F10 (the other fixes) a few times, and compare the fps each way.

The fps in the F9 and F8 capture summaries can mislead. They are averages over the whole capture, so camera movement,
loading and stutters all blend into one number, and F9 switches only the other fixes, never the overhaul. Treat the
captures as diagnostics to attach to a bug report, not as the benchmark.

### The log

The log is `perf-logs\<date-time>_s2prof.log` in the game folder, one per run. It records which fixes installed, the
hotkeys used, capture results and, if the game crashes, where. Captures also write thread and stack files there.

## Settings (`s2prof.ini`)

| Key | Default | Meaning |
|---|---|---|
| `[proxy] multithreaded_device` | 1 | creates the D3D11 device without the single-thread flag; the overhaul's worker threads need it |
| `[proxy] wrap_context` | 1 | hands the game forwarding wrappers of the device and context; the overhaul was tested with it on |
| `[cpu] ccd` | cache | keeps the game's threads on one CCD: `cache` picks the CCD with the largest L3 (the 3D V-Cache CCD of a dual-CCD X3D chip) and does nothing on other CPUs; `off` leaves it to Windows; a number picks that CCD |
| `[s2prof] console` | 0 | 1 opens a window that shows the log live |

## The fixes

| Fix | What it does |
|---|---|
| `overhaul` | splits the draw submission of the shadow cascades, G-buffer, decal and effects views over three worker threads (deferred contexts, replayed in order), with per-thread copies of the engine state draws share; also prepares unit render lists early on idle threads and records views ahead while the workers draw |
| `batchsplit` | splits the render-item task's large batches into ordered chunks on helper threads |
| `walksplit` | runs the draw-time batching walk's filtering and LOD picks on helper threads, recording in the original order |
| `entrysort` | replaces a per-entry sort whose comparator recomputed distances on every comparison |
| `tiegroup` | sorts tied render items so instanced draws group properly |
| `extrasrecord` | records the per-object extras in bulk instead of one call per index |
| `listflag` | stops the parallel render-list append from rewriting a shared flag on every call |
| `instancecopy` | copies per-draw instance records with unaligned 16-byte moves instead of a memcpy call each |
| `itemcopy` | copies 176-byte render items whole |
| `imposterreuse` | reuses imposter uploads already in the geometry buffer this frame |
| `texturecache` | caches the HUD's per-quad texture-name lookups |
| `uiprefetch` | prefetches the UI tree's next nodes during its update walk |
| `powcache` | caches the gamma-to-linear colour conversions (`powf(x, 2.2)`) |
| `luagc` | steps the shared Lua heap's collector once per frame instead of once per Lua state (about 64 times) |
| `focuscache` | asks Windows for the foreground window once per frame instead of on every key query |
| `scenelock` | spins longer on the unit emission lock instead of sleeping in the kernel |
| `cpuplace` | keeps the game's threads on the CCD `[cpu] ccd` names |

The host itself also replaces two engine sorts (render items and effect objects) with stable radix sorts.

## Privacy and safety

The fixes make no network connections and have no remote access. They read `s2prof.ini` and the game's own memory, and
write only the `perf-logs` folder.

## Building

Releases are built by GitHub Actions from the tagged source (`.github/workflows/release.yml`), with WinLibs GCC 16.2.0
(POSIX threads, MSVCRT, r2) checked against its published SHA-256; the build log prints each zip's SHA-256.

To build yourself, with that MinGW-w64 on the path and a POSIX shell (Git Bash works):

```
./build.sh       builds s2prof.dll, proxy/d3d11.dll and modules/*.dll
./package.sh     builds, then writes dist/s2fix_<timestamp>.zip (VERSION=v0.1.0 names it instead)
```

The sort test: `x86_64-w64-mingw32-gcc -O2 -I. -o test_itemsort.exe tests/test_itemsort.c && ./test_itemsort.exe`.

## License

GNU General Public License v3.0; see `LICENSE`.
