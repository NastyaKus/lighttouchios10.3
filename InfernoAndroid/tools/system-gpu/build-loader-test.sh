#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
: "${LDID:?Set LDID}"; : "${OUT:?Set OUT}"
mkdir -p "$OUT"
BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
ARCH="${GUEST_ARCH:-arm64}"
case "$ARCH" in
  arm64) GUEST_CLANG="$BIN/clang"; GUEST_LD="$BIN/ld64.lld" ;;
  arm64e) : "${GUEST_CLANG:?Set a compiler with versioned ptrauth ABI support}"
    : "${GUEST_LD:?Set a linker preserving arm64e ABI version 0}" ;;
  *) echo 'Only arm64 or versioned arm64e is supported' >&2; exit 2 ;;
esac
BASE=(-ffreestanding -O2 -Wall -Wextra -Werror -fno-stack-protector -fno-builtin)
build() {
  local name="$1" mode="$2"; shift 2
  local flags=() objects=() index=0
  if [[ $ARCH == arm64e ]]; then flags+=(-fptrauth-abi-version=0 -DEMBER_NATIVE_ARM64E); fi
  for source in "$@"; do
    if [[ $source == -D* ]]; then flags+=("$source"); continue; fi
    local object="$OUT/$name-$index.o"; index=$((index+1))
    "$GUEST_CLANG" -target "$ARCH-apple-ios14.0" "${BASE[@]}" "${flags[@]}" -c "$source" -o "$object"
    objects+=("$object")
  done
  local link_flags=()
  if [[ $mode == dylib ]]; then link_flags+=(-dylib -install_name /usr/lib/ember/libEmberSystemGLES.dylib); fi
  "$GUEST_LD" -arch "$ARCH" -platform_version ios 14.0 14.0 -L"$HERE/../../gpu-bridge" -lSystem \
    "${link_flags[@]}" "${objects[@]}" -o "$OUT/$name"
  python "$HERE/check-guest-arch.py" "$ARCH" "$OUT/$name"
  "$LDID" -S "$OUT/$name"
  python "$HERE/check-guest-arch.py" "$ARCH" "$OUT/$name"
}
build loader-probe-ios exe -DEGPU_GUEST_FREESTANDING "$HERE/loader-probe.c" \
  "$HERE/../../gpu-bridge/system/wire.c" "$HERE/../../gpu-bridge/system/wire-selftest.c" \
  "$HERE/../../gpu-bridge/stream.c" "$HERE/../../gpu-bridge/protocol.c"
NATIVE_LIBRARY=()
if [[ $ARCH == arm64e ]]; then NATIVE_LIBRARY+=("$HERE/../../gpu-bridge/system/eagl-native.c"); fi
if [[ ${BACKBOARDD_CONSTRUCTOR_TEST:-0} == 1 ]]; then
  [[ $ARCH == arm64e && ${SYSTEM_EXECUTABLE_TEST:-0} == 1 && ${WITH_STOCK_CHROOT:-0} == 1 ]] || { echo 'BB constructor test requires arm64e and disposable System chroot' >&2; exit 2; }
  NATIVE_LIBRARY=(-DEMBER_BACKBOARDD_CONSTRUCTOR_TEST "${NATIVE_LIBRARY[@]}")
fi
build loader-library-ios.dylib dylib "$HERE/loader-library.c" "${NATIVE_LIBRARY[@]}"
build native-env-probe-ios exe "$HERE/native-env-probe.c"
"$LDID" -S"$HERE/../../gpu-bridge/probe.entitlements.plist" "$OUT/native-env-probe-ios"
# The launcher belongs to the small restore environment and remains arm64.
# Only its child and injected library use the requested system ABI.
EXTRA=()
if [[ ${SYSTEM_EXECUTABLE_TEST:-0} == 1 ]]; then EXTRA+=(-DEMBER_SYSTEM_EXECUTABLE); fi
if [[ ${WITH_STOCK_SYSTEM:-0} == 1 ]]; then EXTRA+=(-DEMBER_STOCK_SYSTEM "$HERE/stock-system-bootstrap.c"); fi
if [[ ${WITH_STOCK_CHROOT:-0} == 1 ]]; then
  [[ ${WITH_STOCK_SYSTEM:-0} == 1 ]] || { echo 'Chroot requires the scratch System mount' >&2; exit 2; }
  EXTRA+=(-DEMBER_STOCK_CHROOT "$HERE/stock-system-chroot.c")
fi
"$BIN/clang" -target arm64-apple-ios14.0 "${BASE[@]}" -nostdlib --ld-path="$BIN/ld64.lld" \
  -L"$HERE/../../gpu-bridge" -lSystem -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign \
  "${EXTRA[@]}" "$HERE/loader-launcher.c" -o "$OUT/loader-launcher-ios"
"$LDID" -S"$HERE/../../gpu-bridge/probe.entitlements.plist" "$OUT/loader-launcher-ios"
