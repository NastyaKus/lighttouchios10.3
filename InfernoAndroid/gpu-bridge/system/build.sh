#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${OUT:?Set OUT}"
mkdir -p "$OUT"
${CC:-cc} -std=c11 -O2 -D_DEFAULT_SOURCE -DEGP2_SELFTEST_MAIN -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
  "$HERE/wire.c" "$HERE/../stream.c" "$HERE/../protocol.c" "$HERE/wire-selftest.c" -lcrypto -o "$OUT/egp2-selftest"
# The same vectors with the builtin crypto of the Android self-test client.
${CC:-cc} -std=c11 -O2 -D_DEFAULT_SOURCE -DEGP2_SELFTEST_MAIN -DEGP2_BUILTIN_CRYPTO -Wall -Wextra -Werror ${TEST_CFLAGS:-} \
  "$HERE/wire.c" "$HERE/crypto.c" "$HERE/../stream.c" "$HERE/../protocol.c" "$HERE/wire-selftest.c" -o "$OUT/egp2-selftest-builtin"
"$OUT/egp2-selftest-builtin"
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror ${TEST_CFLAGS:-} "$HERE/crypto.c" "$HERE/crypto-test.c" -lcrypto -o "$OUT/egp2-crypto-test"
"$OUT/egp2-crypto-test"
"$OUT/egp2-selftest"
${CC:-cc} -std=c11 -O2 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
  "$HERE/wire.c" "$HERE/../stream.c" "$HERE/../protocol.c" "$HERE/peer-test.c" -lcrypto -o "$OUT/egp2-peer-linux"
if [[ -n ${ANDROID_NDK_HOME:-} ]]; then
  : "${LDID:?Set LDID}"
  BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
  "$BIN/clang" -target arm64e-apple-ios14.0 -ffreestanding -DEGPU_GUEST_FREESTANDING \
    -O2 -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -nostdlib \
    --ld-path="$BIN/ld64.lld" "$HERE/wire.c" "$HERE/../stream.c" "$HERE/../protocol.c" "$HERE/peer-test.c" \
    -L"$HERE/.." -lSystem -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -o "$OUT/egp2-peer-ios"
  "$LDID" -S"$HERE/../probe.entitlements.plist" "$OUT/egp2-peer-ios"
fi
${CC:-cc} -std=c11 -O2 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
  "$HERE/client.c" "$HERE/wire.c" "$HERE/../stream.c" "$HERE/../protocol.c" "$HERE/client-native-test.c" \
  -lcrypto -o "$OUT/egp2-native-client-linux"
