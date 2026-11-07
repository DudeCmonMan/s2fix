# s2fix for Shogun 2, the original game

## Requirements

- Windows 10 or 11, 64-bit.
- The Steam version with the May 2023 update: `empire.retail.dll` with PE timestamp `0x645e92bb` (SHA-256 starting `022fae2e2da92b59`). If `empire.retail.dll` is a different build, or another mod's copy of it, a warning asks whether to launch. Either way, each fix checks the engine code it depends on and stays off if it was changed; the log lists which.
- DirectX 11 mode. The fixes are for DirectX 11 only: in DirectX 9 mode they don't load, and the game runs unchanged. If the game falls back to DirectX 9 on its own, a message says so and the fixes stay off.
- Tested with an NVIDIA driver.

## Files

```
d3d11.dll        loads s2prof.dll and passes everything else to Windows' own d3d11.dll
s2prof.dll       loads the fixes and provides the hotkeys and the log
s2prof.ini       settings
fixes\           one DLL per fix
```

The game loads `d3d11.dll` only in DirectX 11 mode, as it creates its device. It loads `s2prof.dll`, which holds the game's device creation until the fixes are installed. If s2fix is loaded later (injected while the game runs), a message says that some fixes are off for that session.

## Borderless windowed

The game itself only has a framed window or exclusive fullscreen. s2fix can run it borderless instead, covering the whole monitor. The game renders at the resolution set in its options and is scaled to fill the screen, and a resolution change applies at once. A resolution with another aspect ratio than the monitor's is stretched:

1. In the game's options, choose windowed mode.
2. In `s2prof.ini`, set `borderless=1` under `[window]`.
3. Restart the game.

The cursor stays on the game's monitor while the game has focus, so edge scrolling works with several monitors. Alt+Tab releases it. To let it move freely, set `clip_cursor=0`.

## Settings (`s2prof.ini`)

Settings marked "at start" apply when the game starts. The hotkeys don't switch those.

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
| `[campthrottle] hz` | 30 | at start: how many times a second each of your settlements on the campaign map checks whether it can build something; a whole number, 1 or more (anything else keeps 30). Lower gives more frames on a large empire, and the answer can be up to that fraction of a second old. 0 turns this off and checks every frame, as the game does |
| `[camplabels] hz` | 30 | at start: how many times a second the label of each of your settlements on the campaign map checks whether a rebellion is imminent while the camera moves; a whole number, 1 or more (anything else keeps 30), and the answer can be up to that fraction of a second old. 0 checks every time, as the game does. Other factions' labels skip the check either way, since the game discards its answer for them |
| `[s2prof] console` | 0 | 1 opens a window that shows the log live |
| `[s2prof] ignore_build_check` | 0 | 1 skips the warning shown when `empire.retail.dll` is a different build or another mod's copy of it; each fix still checks the engine code it depends on |

## The fixes

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
| `campeffects` | keeps each region's list of effects between frames: every building slot of your settlements on screen rebuilt the whole list every frame to ask whether it could build something, and the settlement markers and labels rebuilt it for every settlement several times a second to work out its projected figures. A list is reused only while everything it is made from is unchanged, byte for byte |
| `campthrottle` | each of those slots asks whether it can build something at most `[campthrottle] hz` times a second (30 by default) and reuses its last answer in between, so the answer can be up to that fraction of a second old |
| `camplabels` | while the camera moves, every settlement label on screen asked each frame whether a rebellion is imminent, which works out the settlement's whole economy. Other factions' labels skip the question, since the game discards their answer; yours ask at most `[camplabels] hz` times a second (30 by default) and reuse the last answer in between, so it can be up to that fraction of a second old |
| `camphonour` | for every settlement's marker and label, the game works out the honour of the daimyo whose faction owns it, and for one part of it copied that faction's whole list of effects to read a single value; that value is now read from the lists themselves (the result is the same) |
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

Crashes in the game's own code:

| Fix | What it does |
|---|---|
| `soundfilter` | stops the crash at `mss32.dll+0x16eb9`, in the game's sound library: asked to filter a block with no samples while a sound's filter is changing, the library runs off the end of its buffer. That empty block is now skipped; it has nothing to filter, so the sound is the same |
