#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run after :app:compileDebugKotlin. Operates on a caller-provided ramdisk only.
set -euo pipefail
: "${JAVA_HOME:?Set JAVA_HOME to a JDK}"
: "${GRADLE_USER_HOME:?Set GRADLE_USER_HOME to the dependency cache used for the build}"
[[ $# == 2 ]] || { echo 'usage: run.sh stock.im4p output.im4p' >&2; exit 2; }
probe_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
probe_stdlib=("$GRADLE_USER_HOME"/caches/modules-2/files-2.1/org.jetbrains.kotlin/kotlin-stdlib/2.1.20/*/*.jar)
[[ ${#probe_stdlib[@]} == 1 && -f ${probe_stdlib[0]} ]]
probe_build="$probe_root/build/patch-ramdisk-probe"
mkdir -p "$probe_build"
probe_cp="$probe_root/app/build/tmp/kotlin-classes/debug:${probe_stdlib[0]}:$probe_build"
"$JAVA_HOME/bin/javac" -cp "$probe_cp" -d "$probe_build" "$probe_root/tools/patch-ramdisk-probe/Probe.java"
"$JAVA_HOME/bin/java" -cp "$probe_cp" Probe "$1" "$2" "$probe_root/guest/bin/inferno_patcher"
