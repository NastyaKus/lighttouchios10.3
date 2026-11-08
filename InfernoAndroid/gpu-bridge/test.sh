#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${CC:=cc}"
: "${GL_PREFIX:=/usr}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$CC" -std=c11 -O1 -Wall -Wextra -Werror ${TEST_CFLAGS:-} "$HERE/protocol-test.c" "$HERE/protocol.c" -o "$TMP/protocol"
"$TMP/protocol"
"$CC" -std=c11 -D_DEFAULT_SOURCE -O1 -Wall -Wextra -Werror ${TEST_CFLAGS:-} "$HERE/broker-test.c" "$HERE/broker.c" "$HERE/stream.c" "$HERE/protocol.c" -o "$TMP/broker"
"$TMP/broker"
NATIVE="$HERE/../app/src/main/cpp"
"$CC" -std=c11 -O1 -Wall -Wextra -Werror ${TEST_CFLAGS:-} -I"$GL_PREFIX/include" -I"$NATIVE" \
    -L"$GL_PREFIX/lib/x86_64-linux-gnu" "$HERE/executor-test.c" "$HERE/protocol.c" "$HERE/session.c" \
    "$HERE/executor.c" "$HERE/gles-host.c" "$HERE/presentation.c" "$NATIVE/gles_renderer.c" -lEGL -lGLESv2 -o "$TMP/executor"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 LD_LIBRARY_PATH="$GL_PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" "$TMP/executor"
"$CC" -std=c11 -O1 -Wall -Wextra -Werror ${TEST_CFLAGS:-} -I"$GL_PREFIX/include" -I"$NATIVE" \
    -L"$GL_PREFIX/lib/x86_64-linux-gnu" "$HERE/gles-host-test.c" "$HERE/protocol.c" "$HERE/session.c" \
    "$HERE/executor.c" "$HERE/gles-host.c" "$HERE/presentation.c" "$NATIVE/gles_renderer.c" -lEGL -lGLESv2 -o "$TMP/gles-host"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 LD_LIBRARY_PATH="$GL_PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" "$TMP/gles-host"
