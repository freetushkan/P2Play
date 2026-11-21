#!/usr/bin/env bash
# Boost headers in one include tree: ci/boost-headers.sh <version> <out dir>
set -euo pipefail

VERSION="${1:-1.86.0}"
OUT="${2:-/opt/boost-headers}"
WORK="${WORK_DIR:-/tmp/boost-src}"

if [ -f "$OUT/boost/version.hpp" ]; then
    echo "boost headers: $OUT exists"
    exit 0
fi

mkdir -p "$WORK" "$OUT/boost"
cd "$WORK"
curl -fsSL -o boost.tar.xz \
    "https://github.com/boostorg/boost/releases/download/boost-${VERSION}/boost-${VERSION}-cmake.tar.xz"
tar -xf boost.tar.xz
for d in "boost-${VERSION}"/libs/*/include "boost-${VERSION}"/libs/numeric/*/include; do
    if [ -d "$d/boost" ]; then
        cp -r "$d/boost/." "$OUT/boost/"
    fi
done
rm -rf "$WORK"
grep '#define BOOST_LIB_VERSION' "$OUT/boost/version.hpp"
