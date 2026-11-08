#!/usr/bin/env bash
set -euo pipefail
# Usage: tools/swift-parity/run.sh /path/to/Inferno-iOS [swiftc]
# Reference checkout MUST be pinned to the exact original implementation.
project=$(cd "$(dirname "$0")/../.." && pwd)
reference=${1:?Path to MakrSas/Inferno-iOS checkout required}
compiler=${2:-swiftc}
expected=4bc39702871f5a118d96acbad4df7a675ddf14e0
actual=$(git -C "$reference" rev-parse HEAD)
[ "$actual" = "$expected" ] || { echo "Wrong Swift reference commit: $actual" >&2; exit 1; }
work="$project/build/swift-reference"
mkdir -p "$work/guest" "$work/output"
printf 'daemon-test' > "$work/guest/inferno_patcher"
printf 'patcher-test' > "$work/guest/inferno_fs_patcher"
"$compiler" -swift-version 5 "$reference/app/Sources/IMG4.swift" "$reference/app/Sources/Ticket.swift" "$reference/app/Sources/RestoreRamdisk.swift" "$project/tools/swift-parity/main.swift" -o "$work/reference"
"$work/reference" "$project/app/src/test/resources/restore" "$work/output"
for name in ap.der sep.der personalized.img4 job.plist ramdisk.im4p; do
    cmp "$project/app/build/parity/$name" "$work/output/$name"
    sha256sum "$work/output/$name"
done
