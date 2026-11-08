#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# LLVM clang + ld64.lld, Linux/macOS; no Apple SDK/Mac required.
set -euo pipefail
: "${CLANG:=clang}"
: "${MACHO_LD:=ld64.lld}"
guest_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
guest_out=${1:-"$guest_root/bin/inferno_patcher"}
mkdir -p "$(dirname -- "$guest_out")"
"$CLANG" -target arm64-apple-ios14.0 -O2 -Wall -Wextra -Werror \
  -fno-stack-protector -fno-builtin -nostdlib --ld-path="$MACHO_LD" \
  "${2:-$guest_root/patcher-daemon.c}" "$guest_root/launchd-cache.c" -L"$guest_root" -lSystem \
  -Wl,-platform_version,ios,14.0,14.0 -Wl,-adhoc_codesign -Wl,-no_uuid \
  -o "$guest_out"
