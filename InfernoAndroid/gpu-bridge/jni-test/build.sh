#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
: "${JAVA_HOME:?Set JAVA_HOME}"
: "${GL_PREFIX:=/usr}"
: "${CC:=cc}"
HERE="$(cd "$(dirname "$0")" && pwd)"
GPU="$(dirname "$HERE")"
: "${OUT:=$GPU/../build/gpu-jni}"
mkdir -p "$OUT"
NATIVE="$GPU/../app/src/main/cpp"
"$CC" -std=c11 -D_DEFAULT_SOURCE -O2 -fPIC -shared -Wall -Wextra -Werror \
  -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" -I"$GPU" -I"$NATIVE" -I"$GL_PREFIX/include" \
  -L"$GL_PREFIX/lib/x86_64-linux-gnu" "$NATIVE/gpu_probe_jni.c" "$NATIVE/gles_renderer.c" \
  "$GPU/protocol.c" "$GPU/session.c" "$GPU/executor.c" "$GPU/gles-host.c" "$GPU/presentation.c" "$GPU/file-host.c" \
  -lEGL -lGLESv2 -o "$OUT/libember_guest_gpu_probe.so"
"$JAVA_HOME/bin/javac" -d "$OUT" "$HERE/GpuHostNative.java"
