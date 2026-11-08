#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${CC:=cc}"
: "${GL_PREFIX:=/usr}"
: "${OUT:=$HERE/../build/gpu-bridge}"
mkdir -p "$OUT"
NATIVE="$HERE/../app/src/main/cpp"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} -D_DEFAULT_SOURCE \
    "$HERE/guest-probe.c" "$HERE/protocol.c" -o "$OUT/guest-probe-linux"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    -I"$GL_PREFIX/include" -I"$NATIVE" -L"$GL_PREFIX/lib/x86_64-linux-gnu" \
    "$HERE/host-probe.c" "$HERE/file-host.c" "$HERE/protocol.c" "$HERE/session.c" "$HERE/executor.c" "$HERE/gles-host.c" "$HERE/presentation.c" "$NATIVE/gles_renderer.c" \
    -lEGL -lGLESv2 -o "$OUT/host-probe"
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    : "${LDID:?Set LDID to Procursus ldid; see gpu-bridge/README.md}"
    TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
    "$TOOLCHAIN/clang" -target arm64-apple-ios14.0 -ffreestanding -O2 -Wall -Wextra -Werror \
        -DEGPU_GUEST_FREESTANDING -fno-stack-protector -fno-builtin -nostdlib \
        --ld-path="$TOOLCHAIN/ld64.lld" "$HERE/guest-probe.c" "$HERE/protocol.c" \
        -L"$HERE" -lSystem -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -Wl,-no_uuid \
        -o "$OUT/guest-probe-ios"
    "$LDID" -S"$HERE/probe.entitlements.plist" "$OUT/guest-probe-ios"
fi
