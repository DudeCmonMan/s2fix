# s2fix for Shogun 2 — Complete Edition

## Requirements

- Windows 10 or 11, 64-bit.
- The engine build these fixes were made for: `empire.retail.x64.dll` with PE timestamp `0x6ac7c293` (SHA-256 starting `f1b609dffb95c1fa`). If `empire.retail.x64.dll` is a different build, or another mod's copy of it, a warning asks whether to launch. Either way, each fix checks the engine code it depends on and stays off if it was changed; the log lists which.
- Tested on native D3D11 with an NVIDIA driver.

## Files

```
d3d11.dll        loads s2prof.dll and passes everything else to Windows' own d3d11.dll
s2prof.dll       loads the fixes and provides the hotkeys and the log
s2prof.ini       settings
fixes\           one DLL per fix
```

The game loads `d3d11.dll` as it starts its renderer. It loads `s2prof.dll`, which holds the game's device creation until the fixes are installed. If s2fix is loaded later (injected while the game runs), a message says that some fixes are off for that session.

## Settings (`s2prof.ini`)

Settings marked "at start" apply when the game starts. The hotkeys don't switch those.

| Key | Default | Meaning |
|---|---|---|
| `[device] multithreaded_device` | 1 | at start: creates the D3D11 device without the single-thread flag; the overhaul's worker threads need it |
| `[device] wrap_context` | 1 | at start: gives the game forwarding wrappers of the device and context |
| `[cpu] ccd` | auto | which CPUs the game's threads use: `auto` gives the main thread the best core of the CPU's largest cache group (the 3D V-Cache CCD of an X3D chip, the P-cores of an Intel chip) and keeps the other threads on that group when it leaves them 10 CPUs, else on every CPU; a number picks a cache group (0 holds CPU 0); `off` leaves the other threads to Windows, and the main thread still gets its core. `cache`, the old default, means `auto` |
| `[campthrottle] hz` | 30 | at start: how many times a second each of your settlements on the campaign map checks whether it can build something; a whole number, 1 or more (anything else keeps 30). Lower gives more frames on a large empire, and the answer can be up to that fraction of a second old. 0 turns this off and checks every frame, as the game does |
| `[s2prof] console` | 0 | 1 opens a window that shows the log live |
| `[s2prof] ignore_build_check` | 0 | 1 skips the warning shown when `empire.retail.x64.dll` is a different build or another mod's copy of it; each fix still checks the engine code it depends on |

## The fixes

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
| `campeffects` | keeps each region's list of effects between frames: every building slot of your settlements on screen rebuilt the whole list every frame to ask whether it could build something. A list is reused only while everything it is made from is unchanged, byte for byte |
| `campthrottle` | each of those slots asks whether it can build something at most `[campthrottle] hz` times a second (30 by default) and reuses its last answer in between, so the answer can be up to that fraction of a second old |
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
