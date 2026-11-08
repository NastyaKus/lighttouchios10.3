#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
: "${JAVA_HOME:?Set JAVA_HOME}"
: "${GRADLE_USER_HOME:?Set GRADLE_USER_HOME}"
[[ $# == 2 ]] || { echo 'usage: prepare.sh owned-kit-root NEW-scratch-run-directory' >&2; exit 2; }
PROJECT="$(cd "$(dirname "$0")/../.." && pwd)"
STDLIB=("$GRADLE_USER_HOME"/caches/modules-2/files-2.1/org.jetbrains.kotlin/kotlin-stdlib/2.1.20/*/*.jar)
[[ ${#STDLIB[@]} == 1 && -f ${STDLIB[0]} ]]
BUILD="$PROJECT/build/gpu-lab-probe";mkdir -p "$BUILD"
CP="$PROJECT/app/build/tmp/kotlin-classes/debug:${STDLIB[0]}:$BUILD"
"$JAVA_HOME/bin/javac" -cp "$CP" -d "$BUILD" "$PROJECT/tools/gpu-lab-probe/Prepare.java"
"$JAVA_HOME/bin/java" -cp "$CP" Prepare "$1" "$2" "$PROJECT/gpu-bridge/bin/eagl-api-probe-ios" "$PROJECT/gpu-bridge/bin/libEmberGLES.dylib"
