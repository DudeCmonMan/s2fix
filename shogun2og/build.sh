#!/bin/sh
set -e
cd "$(dirname "$0")"
# The game is 32-bit: everything is built with an i686 MinGW-w64 (WinLibs GCC 16.2.0, POSIX threads, MSVCRT, r2).
# -static: a POSIX-threads MinGW implements __thread through libwinpthread, a DLL the game folder doesn't have.
# -s: no symbols or debug information in what ships.
# -msse2 -mfpmath=sse -ffp-contract=off: float math rounds as the engine's SSE code does (x87 would not).
# -mstackrealign: the engine calls hooks with a 4-byte aligned stack.
CC=i686-w64-mingw32-gcc
# Known-size copies up to 512 bytes inline as SSE2 loops: msvcrt's memcpy takes rep movsd for misaligned ones, which
# the 312-byte render items mostly are. Variable sizes stay library calls.
CFLAGS="-O2 -Wall -Wextra -D__USE_MINGW_ANSI_STDIO=1 -static -s -msse2 -mfpmath=sse -ffp-contract=off -mstackrealign -mmemcpy-strategy=vector_loop:512:noalign,libcall:-1:noalign"
# The debug report names the version it came from: the release workflow passes the tag.
$CC $CFLAGS -DS2FIX_VERSION="\"${VERSION:-local build}\"" -shared -o s2prof.dll s2prof.c hook.c report.c -lpsapi -lntdll -lversion -lole32 -lgdi32
# The proxy's export table comes from its export list.
{ echo "LIBRARY d3d11"; echo "EXPORTS"; sed -nE 's/^ *(STUB|OWN)\(([A-Za-z0-9_]+), ([0-9]+)\).*/    \2=proxy_\2 @\3/p' proxy/exports.h; } > proxy/d3d11.def
$CC $CFLAGS -shared -o proxy/d3d11.dll proxy/d3d11.c proxy/d3d11.def
for module in modules/*.c; do $CC $CFLAGS -shared -o "${module%.c}.dll" "$module" -lntdll -lsynchronization; done
