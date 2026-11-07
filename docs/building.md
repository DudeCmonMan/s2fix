# Building s2fix

Releases are built by GitHub Actions from the tagged source (`.github/workflows/release.yml`), with WinLibs GCC 16.2.0 (POSIX threads, MSVCRT, r2) checked against its published SHA-256. The build log prints each zip's SHA-256.

To build yourself, put both MinGW-w64 compilers on the path (`x86_64-w64-mingw32-gcc` for the Complete Edition, `i686-w64-mingw32-gcc` for the original game) and use a POSIX shell (Git Bash works):

```
./build.sh       builds both games: shogun2ce/ and shogun2og/
./package.sh     builds, then writes dist/s2fix_shogun2_complete-edition_<version>.zip and
                 dist/s2fix_shogun2_original_<version>.zip
                 (VERSION=v0.2.1 names them; a local build gets a timestamp)
```

Every DLL gets Windows version info (`versioninfo.sh`, with the windres beside each compiler), filled from VERSION: a tag such as v0.5.0 gives 0.5.0.0, a `-pre-release` tag is marked pre-release, and any other build gets 0.0.0.0.

Each game's folder also has its own `build.sh` and `package.sh`. The unit tests are in `shogun2ce/tests/` and `shogun2og/tests/`; `.github/workflows/build.yml` shows how to build and run them.
