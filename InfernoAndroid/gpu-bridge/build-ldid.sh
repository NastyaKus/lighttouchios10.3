#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Requires git, make, C++, libssl-dev, libplist-dev, pkg-config on Linux.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${OUT:=$HERE/../build/ldid}"
mkdir -p "$OUT"
if [ ! -d "$OUT/src/.git" ]; then git init -q "$OUT/src"; fi
git -C "$OUT/src" fetch -q --depth 1 https://github.com/ProcursusTeam/ldid.git af86971ae72ec3ed3d0a699107c4e882324c941b
git -C "$OUT/src" -c advice.detachedHead=false checkout -q FETCH_HEAD
make -C "$OUT/src" -j2 VERSION=2.1.5-procursus7
echo "$OUT/src/ldid"
