#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${OUT:?Set OUT}"; : "${GL_PREFIX:=/usr}"
mkdir -p "$OUT"
NATIVE="$HERE/../../app/src/main/cpp"
${CC:-cc} -std=c11 -O1 -Wall -Wextra -Werror ${TEST_CFLAGS:-} -I"$GL_PREFIX/include" -I"$NATIVE" \
  -L"$GL_PREFIX/lib/x86_64-linux-gnu" "$HERE/surface-test.c" "$HERE/surface.c" "$NATIVE/gles_renderer.c" \
  -lEGL -lGLESv2 -o "$OUT/system-surface-test"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 LD_LIBRARY_PATH="$GL_PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" "$OUT/system-surface-test"
