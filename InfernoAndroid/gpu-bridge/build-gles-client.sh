#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${CC:=cc}"
: "${OUT:=$HERE/../build/gles-client}"
mkdir -p "$OUT"
"$CC" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    -fPIC -shared "$HERE/gles-client.c" "$HERE/protocol.c" "$HERE/stream.c" -o "$OUT/libEmberGLES.so"
"$CC" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    "$HERE/gles-api-probe.c" -L"$OUT" -lEmberGLES '-Wl,-rpath,$ORIGIN' -pthread -o "$OUT/gles-api-probe-linux"
"$CC" -std=c11 -D_DEFAULT_SOURCE -DEGPU_PRESENT_LINUX_PROBE -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    "$HERE/gles-api-probe.c" "$HERE/present-probe.c" -L"$OUT" -lEmberGLES '-Wl,-rpath,$ORIGIN' -pthread -o "$OUT/present-api-probe-linux"
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    : "${LDID:?Set LDID}"
    TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
    "$TOOLCHAIN/clang" -target arm64-apple-ios14.0 -ffreestanding -DEGPU_GUEST_FREESTANDING \
        -O2 -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -nostdlib -dynamiclib \
        --ld-path="$TOOLCHAIN/ld64.lld" "$HERE/gles-client.c" "$HERE/protocol.c" "$HERE/stream.c" "$HERE/eagl-adapter.c" -L"$HERE" -lSystem \
        -Wl,-install_name,/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-11.04-012.2-P.bin \
        -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -Wl,-no_uuid -o "$OUT/libEmberGLES.dylib"
    "$LDID" -S "$OUT/libEmberGLES.dylib"
    "$TOOLCHAIN/clang" -target arm64-apple-ios14.0 -ffreestanding -DEGPU_GUEST_FREESTANDING \
        -O2 -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -nostdlib --ld-path="$TOOLCHAIN/ld64.lld" \
        "$HERE/gles-api-probe.c" "$OUT/libEmberGLES.dylib" -L"$HERE" -lSystem \
        -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -Wl,-no_uuid -o "$OUT/gles-api-probe-ios"
    "$LDID" -S"$HERE/probe.entitlements.plist" "$OUT/gles-api-probe-ios"
    "$TOOLCHAIN/clang" -target arm64-apple-ios14.0 -ffreestanding -DEGPU_GUEST_FREESTANDING \
        -O2 -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -nostdlib --ld-path="$TOOLCHAIN/ld64.lld" \
        "$HERE/gles-api-probe.c" "$HERE/eagl-api-probe.m" "$OUT/libEmberGLES.dylib" -L"$HERE" -lSystem -lobjc \
        -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -Wl,-no_uuid -o "$OUT/eagl-api-probe-ios"
    "$LDID" -S"$HERE/probe.entitlements.plist" "$OUT/eagl-api-probe-ios"
    "$TOOLCHAIN/clang" -target arm64-apple-ios14.0 -ffreestanding -DEGPU_GUEST_FREESTANDING -DEGPU_PRESENT_PROBE \
        -O2 -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -nostdlib --ld-path="$TOOLCHAIN/ld64.lld" \
        "$HERE/gles-api-probe.c" "$HERE/eagl-api-probe.m" "$HERE/present-probe.c" "$OUT/libEmberGLES.dylib" -L"$HERE" -lSystem -lobjc \
        -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -Wl,-no_uuid -o "$OUT/eagl-present-probe-ios"
    "$LDID" -S"$HERE/probe.entitlements.plist" "$OUT/eagl-present-probe-ios"
fi
