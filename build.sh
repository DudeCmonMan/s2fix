#!/bin/sh
set -e
cd "$(dirname "$0")"
# Each game builds in its own folder with its own compiler: x86_64-w64-mingw32-gcc for the Complete Edition,
# i686-w64-mingw32-gcc for the original. Both must be on the path.
./shogun2ce/build.sh
./shogun2og/build.sh
