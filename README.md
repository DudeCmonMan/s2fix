# s2fix: performance fixes for Total War: Shogun 2

Runtime fixes that make battles in Total War: Shogun 2 render faster, and its campaign map and end turns run faster, in both the Complete Edition and the original 2011 game. No game file is changed: a proxy DLL next to the game loads the fixes, and they patch the engine in memory only.

This is an unofficial project, not affiliated with or endorsed by SEGA or Creative Assembly.

## Install

1. Download the package for your game:

   | Game | Package | Unzip next to |
   |---|---|---|
   | Shogun 2 — Complete Edition (64-bit) | `s2fix_shogun2_complete-edition_<version>.zip` | `shogun2_complete_edition.exe` |
   | Shogun 2, the original game (32-bit) | `s2fix_shogun2_original_<version>.zip` | `Shogun2.exe` |

2. Unzip it into the game folder, next to the file in the last column.
3. Start the game as usual.

Every fix is on by default and there is nothing to set up. You need Windows 10 or 11, 64-bit. The original game must run in DirectX 11 mode: in DirectX 9 the fixes don't load and the game runs unchanged.

If your game's engine isn't the build s2fix was made for, or another mod has replaced it, a warning asks whether to launch. Any fix whose engine code was changed stays off.

To uninstall, delete `d3d11.dll`, `s2prof.dll`, `s2prof.ini`, `s2fix-README.md`, `s2fix-LICENSE.txt`, the `fixes\` folder and the `perf-logs\` folder.

### Linux with Proton

It should work out of the box.

Proton/Wine loads `s2prof.dll` automatically. It should also ordinarily load the mod's `d3d11.dll` over
Proton/Wine's own builtin version.

However, in order to ensure that it does load the correct files, you may want to add `d3d11` to the
`WINEDLLOVERRIDES` option in the game's launcher options just for peace of mind:

```sh
WINEDLLOVERRIDES="d3d11=n,b" %command%
```

### Borderless windowed (original game)

1. In the game's options, choose windowed mode.
2. In `s2prof.ini`, set `borderless=1` under `[window]`.
3. Restart the game.

## Hotkeys

You don't need any hotkeys to play. Most are for comparing the game with the fixes off and on.

| Hotkey | Does |
|---|---|
| Ctrl+Shift+F10 | switches all fixes off and on (on at start) |
| Ctrl+Shift+F9 | 20-second A/B capture: all fixes switch off and on every 2 s; the summary goes to the log |
| Ctrl+Shift+F8 | 20-second capture without switching anything |
| Ctrl+Shift+F6 | debug report (see "Reporting a problem") |
| Ctrl+Shift+F5 | a snapshot, then the game rebuilds its graphics (see "When the picture goes wrong") |

To compare fps, turn on an fps counter (the Steam overlay's, for example), keep the camera still in a busy battle, and press Ctrl+Shift+F10 a few times. The capture summaries average everything in the capture, so they are for bug reports, not benchmarks.

## Reporting a problem

1. In a battle, put the camera somewhere busy and keep it still.
2. Press Ctrl+Shift+F6. You hear a beep and a "Snapshot Running" banner appears. Don't touch the camera until it goes away and you hear a second beep, about 3 minutes later.
3. Attach `perf-logs\report_<date-time>.zip` from the game folder to your issue.

The report holds your system details, the game's settings, the fixes, the program modules loaded into the game, timings with the fixes on and off, and screenshots of the game (never your desktop). Your user name and computer name are replaced, and nothing is sent anywhere: you decide what to share. Each run also writes a log, `perf-logs\<date-time>_s2prof.log`.

## When the picture goes wrong

If textures, shadows or other graphics look broken and stay that way, press Ctrl+Shift+F5. s2fix saves `perf-logs\snapshot_<date-time>.zip`, then the game rebuilds its graphics: the loading screen shows for a moment and the game carries on. If the problem comes back, attach the snapshot to your issue. On the Complete Edition, several resets in a row can make shadows flicker as the camera moves.

## Privacy and safety

The fixes make no network connections and have no remote access. They read `s2prof.ini` and the game's own memory, and write only the `perf-logs` folder.

## More detail

The `docs` folder of [the source repository](https://github.com/DudeCmonMan/s2fix) has the details:

- [docs/shogun_ce.md](docs/shogun_ce.md): the Complete Edition's requirements, settings and every fix.
- [docs/shogun_og.md](docs/shogun_og.md): the same for the original game, with borderless windowed in full.
- [docs/building.md](docs/building.md): building s2fix yourself.

## License

GNU General Public License v3.0; see `LICENSE`.
