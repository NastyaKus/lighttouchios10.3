#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Static arm64 Android build of ember-import, for import.py to push over adb.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
OUT="${OUT:-$HERE}"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --target=aarch64-linux-android29 \
  -static -O2 -s -Wall -Wextra -Werror "$HERE/ember-import.c" -o "$OUT/ember-import-arm64"
sha256sum "$OUT/ember-import-arm64"
