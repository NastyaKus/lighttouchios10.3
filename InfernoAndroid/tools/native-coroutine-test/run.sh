#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "$0")/../.." && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
: "${QEMU_AARCH64:=qemu-system-aarch64}"
compiler="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
work="$project/build/native-coroutine-test"
mkdir -p "$work"
"$compiler" -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
    -mbranch-protection=standard \
    -I"$project/build/inferno-src/util" "$project/tools/native-coroutine-test/main.c" \
    -c -o "$work/main.o"
"$compiler" -c -march=armv8.5-a "$project/tools/native-coroutine-test/start.S" -o "$work/start.o"
"$compiler" -nostdlib -static -Wl,-e,_start -Wl,-Ttext=0x40080000 "$work/start.o" "$work/main.o" -o "$work/test.elf"
for cpu in cortex-a57 max; do
    echo "AArch64 coroutine regression: $cpu"
    timeout 30 "$QEMU_AARCH64" -M virt,virtualization=off -cpu "$cpu" -m 64M -smp 1 \
        -display none -serial none -monitor none -semihosting-config enable=on,target=native \
        -kernel "$work/test.elf"
done
