#!/bin/sh
set -e
cd "$(dirname "$0")"
# One zip per game in dist/: s2fix_shogun2ce_<VERSION>.zip and s2fix_shogun2og_<VERSION>.zip (the release workflow
# passes the tag as VERSION; a local build gets a timestamp).
VERSION=${VERSION:-$(date +%s)}
export VERSION
export DIST="$(pwd)/dist"
./shogun2ce/package.sh
./shogun2og/package.sh
