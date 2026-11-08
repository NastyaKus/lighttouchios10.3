#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Caller supplies stock IM4P; only cached GPL guest programs are added.
set -euo pipefail
: "${JAVA_HOME:?Set JAVA_HOME to a JDK}"
: "${GRADLE_USER_HOME:?Set GRADLE_USER_HOME}"
[[ $# == 3 || $# == 4 || $# == 5 ]] || { echo 'usage: build-with-tools.sh stock.im4p injected.im4p patch-boot.im4p [GPL-daemon-override] [GPL-patcher-slot-override]' >&2; exit 2; }
probe_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
probe_stdlib=("$GRADLE_USER_HOME"/caches/modules-2/files-2.1/org.jetbrains.kotlin/kotlin-stdlib/2.1.20/*/*.jar)
[[ ${#probe_stdlib[@]} == 1 && -f ${probe_stdlib[0]} ]]
probe_build="$probe_root/build/patch-ramdisk-probe"
mkdir -p "$probe_build"
probe_cp="$probe_root/app/build/tmp/kotlin-classes/debug:${probe_stdlib[0]}:$probe_build"
"$JAVA_HOME/bin/javac" -cp "$probe_cp" -d "$probe_build" "$probe_root/tools/patch-ramdisk-probe/BuildRamdisk.java"
"$JAVA_HOME/bin/java" -cp "$probe_cp" BuildRamdisk "$1" "$2" "$3" \
  "${4:-$probe_root/app/build/generated/guest-assets/guest/inferno_patcher}" \
  "${5:-$probe_root/app/build/generated/guest-assets/guest/inferno_fs_patcher}"
