#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${JAVA_HOME:?Set JAVA_HOME}"; : "${GRADLE_USER_HOME:?Set GRADLE_USER_HOME}"
: "${SIGNER_DIR:?Set SIGNER_DIR to Linux JNI verification build}"
: "${OUT:?Set OUT to private build directory}"
[[ $# == 2 ]] || { echo 'usage: prepare-backboardd.sh stock-backboardd NEW-private-output' >&2; exit 2; }
mkdir -p "$OUT"
LIB=("$GRADLE_USER_HOME"/caches/modules-2/files-2.1/org.jetbrains.kotlin/kotlin-stdlib/2.1.20/*/*.jar)
[[ ${#LIB[@]} == 1 && -f ${LIB[0]} ]]
CP="$HERE/../../app/build/tmp/kotlin-classes/debug:${LIB[0]}:$OUT"
"$JAVA_HOME/bin/javac" -cp "$CP" -d "$OUT" "$HERE/PrepareBackboardd.java"
umask 077
"$JAVA_HOME/bin/java" -Djava.library.path="$SIGNER_DIR" -cp "$CP" PrepareBackboardd "$1" "$2"
