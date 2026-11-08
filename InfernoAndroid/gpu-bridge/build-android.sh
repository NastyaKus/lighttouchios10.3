#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Optional command executor library, not linked into the production VM yet.
set -euo pipefail
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${OUT:=$HERE/../build/gpu-bridge-android}"
mkdir -p "$OUT"
NATIVE="$HERE/../app/src/main/cpp"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang" \
    -std=c11 -O2 -fPIC -shared -Wall -Wextra -Werror -I"$NATIVE" \
    "$HERE/protocol.c" "$HERE/session.c" "$HERE/executor.c" "$HERE/file-host.c" "$HERE/gles-host.c" "$HERE/presentation.c" \
    "$NATIVE/gles_renderer.c" -lEGL -lGLESv3 -landroid \
    -Wl,-z,max-page-size=16384 -o "$OUT/libember_guest_gpu_probe.so"
