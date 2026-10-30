#!/bin/sh
set -e
cd "$(dirname "$0")"
# Each game has its own compiler: x86_64-w64-mingw32-gcc and i686-w64-mingw32-gcc must both be on the path.
./shogun2ce/build.sh
./shogun2og/build.sh
