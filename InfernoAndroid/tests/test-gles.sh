#!/usr/bin/env bash
set -euo pipefail
BASE="$(cd "$(dirname "$0")/.." && pwd)"
: "${CC:=cc}"
: "${GL_PREFIX:=/usr}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$CC" -std=c11 -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    -I"$GL_PREFIX/include" -I"$BASE/app/src/main/cpp" \
    "$BASE/tests/gles-renderer-test.c" "$BASE/app/src/main/cpp/gles_renderer.c" \
    -L"$GL_PREFIX/lib/x86_64-linux-gnu" -Wl,--wrap=eglSwapBuffers -lEGL -lGLESv2 -o "$TMP/test"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
    LD_LIBRARY_PATH="$GL_PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" "$TMP/test"
