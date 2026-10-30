#!/bin/sh
set -e
cd "$(dirname "$0")"
# On Windows python3 is a Store alias that only prints how to install Python.
PYTHON=python3
"$PYTHON" -c '' 2>/dev/null || PYTHON=python
./build.sh
# The root package.sh passes DIST and VERSION; run alone, the zip gets a timestamp.
DIST=${DIST:-../dist}
mkdir -p "$DIST"
ZIP="$DIST/s2fix_shogun2_original_${VERSION:-$(date +%s)}.zip"
# Unzipped into the game folder, beside Shogun2.exe.
"$PYTHON" -I -c 'import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as archive:
    archive.write("proxy/d3d11.dll", "d3d11.dll")
    archive.write("s2prof.dll", "s2prof.dll")
    archive.write("s2prof.ini", "s2prof.ini")
    archive.write("../README.md", "s2fix-README.md")
    archive.write("../LICENSE", "s2fix-LICENSE.txt")
    for path in sorted(sys.argv[2:]): archive.write(path, "fixes/" + os.path.basename(path))' "$ZIP" modules/*.dll
echo "$(cd "$DIST" && pwd)/$(basename "$ZIP")"
