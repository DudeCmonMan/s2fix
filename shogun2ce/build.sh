#!/bin/sh
set -e
cd "$(dirname "$0")"
# -static: a POSIX-threads MinGW (WinLibs) implements __thread through libwinpthread, a DLL the game folder doesn't have.
# -s: no symbols or debug information (the toolchain's own included) in what ships.
CFLAGS="-O2 -Wall -Wextra -D__USE_MINGW_ANSI_STDIO=1 -static -s"
x86_64-w64-mingw32-gcc $CFLAGS -shared -o s2prof.dll s2prof.c sortfix.c -lpsapi -lntdll
# The proxy's export table comes from its export list.
{ echo "LIBRARY d3d11"; echo "EXPORTS"; sed -nE 's/^ *(STUB|OWN)\(([A-Za-z0-9_]+), ([0-9]+)\).*/    \2=proxy_\2 @\3/p' proxy/exports.h; echo "    wrapped_context_vtable DATA"; } > proxy/d3d11.def
x86_64-w64-mingw32-gcc $CFLAGS -shared -o proxy/d3d11.dll proxy/d3d11.c proxy/wrap.c proxy/d3d11.def
for module in modules/*.c; do x86_64-w64-mingw32-gcc $CFLAGS -shared -o "${module%.c}.dll" "$module" -lntdll -lsynchronization; done
