#!/usr/bin/env bash
# Boost headers in one include tree, with patches/boost applied:
# ci/boost-headers.sh <version> <out dir>
set -euo pipefail

VERSION="${1:-1.86.0}"
OUT="${2:-/opt/boost-headers}"
WORK="${WORK_DIR:-/tmp/boost-src}"
PATCHES="$(cd "$(dirname "$0")/../patches/boost" && pwd)"

if [ -f "$OUT/boost/version.hpp" ]; then
    echo "boost headers: $OUT exists"
else
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
fi

for p in "$PATCHES"/*.patch; do
    [ -e "$p" ] || continue
    if patch -d "$OUT" -p1 -R --dry-run -s -f < "$p" > /dev/null 2>&1; then
        echo "boost patch: $(basename "$p") already applied"
    else
        patch -d "$OUT" -p1 -s -f < "$p"
        echo "boost patch: $(basename "$p") applied"
    fi
done
grep '#define BOOST_LIB_VERSION' "$OUT/boost/version.hpp"
