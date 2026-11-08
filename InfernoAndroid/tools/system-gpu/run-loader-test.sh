#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${JAVA_HOME:?Set JAVA_HOME}"; : "${GRADLE_USER_HOME:?Set GRADLE_USER_HOME}"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"; : "${LDID:?Set pinned LDID path}"
: "${SIGNER_DIR:?Set Linux JNI signer directory}"
: "${OUT:?Set NEW private output directory}"; : "${QEMU:?Set Linux QEMU path}"
: "${BOOT_INPUTS:?Set caller-owned Inferno inputs}"; : "${KERNEL:?Set caller-owned kernelcache}"
: "${STOCK_RAMDISK:?Set caller-owned stock IM4P ramdisk}"
[[ ! -e "$OUT" && ! -L "$OUT" ]] || { echo 'OUT must not exist' >&2; exit 2; }
python - "$OUT" "$BOOT_INPUTS" <<'PY'
import sys
from pathlib import Path
out,inputs=map(lambda p:Path(p).resolve(),sys.argv[1:])
if out.is_relative_to(inputs) or inputs.is_relative_to(out):raise SystemExit('OUT must be outside the input kit')
PY
umask 077
MODE="${MODE:-loader-control}"
BUILD=(env SYSTEM_EXECUTABLE_TEST=0 BACKBOARDD_CONSTRUCTOR_TEST=0 WITH_STOCK_SYSTEM=0 WITH_STOCK_CHROOT=0 GUEST_ARCH=arm64)
PROBE=loader-probe-ios
EXPECT=--expect-system-loader
EXTRA=()
case "$MODE" in
  loader-control) ;;
  native-loader|native-eagl|backboardd)
    : "${GUEST_CLANG:?Set versioned arm64e compiler}"; : "${GUEST_LD:?Set compatible ld64}"
    : "${SYSTEM_NBD_SOCKET:?Set local read-only decoded System export}"
    : "${QEMU_IMG:?Set qemu-img}"
    BUILD+=(GUEST_ARCH=arm64e WITH_STOCK_SYSTEM=1 WITH_STOCK_CHROOT=1)
    EXTRA=(--memory-gb 4 --readonly-system-socket "$SYSTEM_NBD_SOCKET" --scratch-system-overlay --qemu-img "$QEMU_IMG")
    if [[ -n ${SYSTEM_NBD_STUB:-} ]]; then EXTRA+=(--readonly-system-stub "$SYSTEM_NBD_STUB"); fi
    if [[ $MODE == native-eagl ]]; then PROBE=native-env-probe-ios; EXPECT=--expect-system-env; fi
    if [[ $MODE == backboardd ]]; then
      : "${STOCK_BACKBOARDD:?Set exact stock 18A5351d executable}"
      BUILD+=(SYSTEM_EXECUTABLE_TEST=1 BACKBOARDD_CONSTRUCTOR_TEST=1)
      EXPECT=--expect-backboardd-loader
    fi ;;
  *) echo 'MODE must be loader-control, native-loader, native-eagl or backboardd' >&2; exit 2 ;;
esac
"${BUILD[@]}" "$HERE/build-loader-test.sh"
LIB=("$GRADLE_USER_HOME"/caches/modules-2/files-2.1/org.jetbrains.kotlin/kotlin-stdlib/2.1.20/*/*.jar)
[[ ${#LIB[@]} == 1 && -f ${LIB[0]} ]]
CP="$HERE/../../app/build/tmp/kotlin-classes/debug:${LIB[0]}:$OUT"
"$JAVA_HOME/bin/javac" -cp "$CP" -d "$OUT" "$HERE/PrepareLoader.java" "$HERE/PrepareTrustCache.java"
ARGS=("$OUT/$PROBE")
if [[ $MODE == backboardd ]]; then ARGS=(--stock-backboardd "$STOCK_BACKBOARDD"); fi
"$JAVA_HOME/bin/java" -Djava.library.path="$SIGNER_DIR" -cp "$CP" PrepareLoader \
  "${ARGS[@]}" "$OUT/loader-library-ios.dylib" "$OUT/bundle.bin"
python - "$BOOT_INPUTS" "$OUT/inputs" <<'PY'
from pathlib import Path
import shutil,sys
source,dest=map(Path,sys.argv[1:]);dest.mkdir()
paths=['AppleSEPROM-Cebu-B1','InfernoData/root_ticket.der','InfernoData/sep-firmware.n104.RELEASE.new.img4',
 'InfernoData/Restore/Firmware/all_flash/DeviceTree.n104ap.im4p','InfernoData/Restore/Firmware/038-44135-124.dmg.trustcache']
for path in paths:
 p=source/path
 if not p.is_file():p=source/Path(path).name
 if not p.is_file():raise SystemExit('Missing caller input '+path)
 shutil.copyfile(p,dest/p.name)
PY
"$JAVA_HOME/bin/java" -cp "$CP" PrepareTrustCache "$OUT/inputs/038-44135-124.dmg.trustcache" \
  "$OUT/augmented.trustcache" "$OUT/bundle.bin.executable" "$OUT/loader-library-ios.dylib" "$OUT/loader-launcher-ios"
mv "$OUT/augmented.trustcache" "$OUT/inputs/038-44135-124.dmg.trustcache"
"$HERE/../patch-ramdisk-probe/build-with-tools.sh" "$STOCK_RAMDISK" "$OUT/injected.im4p" "$OUT/probe.im4p" \
  "$OUT/loader-launcher-ios" "$OUT/bundle.bin"
python "$HERE/../patch-boot-probe/run.py" --qemu "$QEMU" --inputs "$OUT/inputs" --kernel "$KERNEL" \
  --ramdisk "$OUT/probe.im4p" "$EXPECT" "${EXTRA[@]}" --work "$OUT/boot"
python - "$OUT/boot/result.json" "$MODE" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]));mode=sys.argv[2]
if mode in ('loader-control','native-loader') and not r.get('wire_crypto_verified'):raise SystemExit('Wire/crypto marker absent')
if mode=='native-loader' and not r.get('versioned_arm64e_loaded'):raise SystemExit('Versioned arm64e was not loaded')
if mode=='native-eagl' and not (r.get('stock_system_cache_verified') and r.get('native_eagl_interposition_verified')):
 raise SystemExit('Real stock System EAGL interposition was not verified')
PY
