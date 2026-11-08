#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${CC:=cc}"
: "${OUT:=$HERE/../build/gles-client}"
mkdir -p "$OUT"
"$CC" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    "$HERE/broker-launcher.c" "$HERE/broker.c" "$HERE/mailbox.c" "$HERE/stream.c" "$HERE/protocol.c" -o "$OUT/broker-launcher-linux"
"$CC" -std=c11 -D_DEFAULT_SOURCE -DEGPU_PRESENT_LINUX_PROBE -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
    "$HERE/gles-api-probe.c" "$HERE/present-probe.c" "$HERE/broker-client-probe.c" \
    -L"$OUT" -lEmberGLES '-Wl,-rpath,$ORIGIN' -pthread -o "$OUT/broker-client-linux"
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
    : "${LDID:?Set LDID}"
    TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
    COMMON=(-target arm64-apple-ios14.0 -ffreestanding -DEGPU_GUEST_FREESTANDING -O2 -Wall -Wextra -Werror
        -fno-stack-protector -fno-builtin -nostdlib --ld-path="$TOOLCHAIN/ld64.lld"
        -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -Wl,-no_uuid -L"$HERE" -lSystem)
    "$TOOLCHAIN/clang" "${COMMON[@]}" "$HERE/broker-launcher.c" "$HERE/broker.c" "$HERE/mailbox.c" \
        "$HERE/stream.c" "$HERE/protocol.c" -o "$OUT/broker-launcher-ios"
    "$LDID" -S"$HERE/probe.entitlements.plist" "$OUT/broker-launcher-ios"
    "$TOOLCHAIN/clang" "${COMMON[@]}" -DEGPU_PRESENT_PROBE -DEGPU_BROKER_PROBE \
        "$HERE/gles-api-probe.c" "$HERE/eagl-api-probe.m" "$HERE/present-probe.c" "$HERE/broker-client-probe.c" \
        "$HERE/gles-client.c" "$HERE/eagl-adapter.c" "$HERE/protocol.c" "$HERE/stream.c" -lobjc -o "$OUT/broker-client-ios"
    # No raw-disk or platform entitlements for the mobile client.
    "$LDID" -S "$OUT/broker-client-ios"
fi
