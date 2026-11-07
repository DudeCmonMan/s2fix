#!/bin/sh
set -e
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
GAME="Total War: Shogun 2 (Complete Edition)"
. ../versioninfo.sh
# -static: a POSIX-threads MinGW (WinLibs) implements __thread through libwinpthread, a DLL the game folder doesn't have.
CFLAGS="-O2 -Wall -Wextra -Werror -D__USE_MINGW_ANSI_STDIO=1 -static -s"
versioninfo s2prof.dll "s2fix loader for $GAME"
$CC $CFLAGS -DS2FIX_VERSION="\"${VERSION:-local build}\"" -shared -o s2prof.dll s2prof.c sortfix.c hook.c wrap.c report.c graphicsreset.c s2prof.version.o -lpsapi -lntdll -lversion -lole32 -lgdi32 -limm32 -lwevtapi
# The proxy's export table comes from its export list.
{ echo "LIBRARY d3d11"; echo "EXPORTS"; sed -nE 's/^ *(STUB|OWN)\(([A-Za-z0-9_]+), ([0-9]+)\).*/    \2=proxy_\2 @\3/p' proxy/exports.h; } > proxy/d3d11.def
versioninfo proxy/d3d11.dll "s2fix Direct3D 11 proxy for $GAME"
$CC $CFLAGS -shared -o proxy/d3d11.dll proxy/d3d11.c proxy/d3d11.def proxy/d3d11.version.o
for module in modules/*.c; do
    versioninfo "${module%.c}.dll" "s2fix $(basename "$module" .c) fix for $GAME"
    $CC $CFLAGS -shared -o "${module%.c}.dll" "$module" "${module%.c}.version.o" -lntdll -lsynchronization
done
