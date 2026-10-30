#!/bin/sh
set -e
cd "$(dirname "$0")"
# The release workflow passes the tag as VERSION; a local build gets a timestamp.
VERSION=${VERSION:-$(date +%s)}
export VERSION
export DIST="$(pwd)/dist"
./shogun2ce/package.sh
./shogun2og/package.sh
