# Patch-only guest daemon

GPL-3.0-or-later, adapted from MakrSas/Inferno-iOS @ 4bc3970
(app/guest/patcher-daemon.c). Source, minimal public ABI declarations and a
handwritten TAPI link interface are included; no Apple SDK/library code.

Build on Linux using LLVM clang + ld64.lld:

```sh
CLANG=/path/to/clang MACHO_LD=/path/to/ld64.lld ./build.sh
```

bin/inferno_patcher is a redistributable ad-hoc-signed arm64 Mach-O built with
LLVM 19 (Android NDK 28.2.13676358 Linux toolchain). It imports /usr/lib/libSystem.B.dylib
from the guest. The app build checks its SHA-256. A different LLVM may produce
a different binary; review/retest and update the pin when rebuilding it.
The pinned upstream filesystem patcher still comes from the verified v0.3.1 IPA.

The production daemon has no runtime test/path overrides. EMBER_TEST_ROOT is
compile-time only and used by the host/synthetic guest tests. Never inject the
synthetic probe executable into a real restore attempt: its device files, cache,
mount commands and patcher are fake. The optional probe remounts the scratch
ramdisk itself read-write to create its test files; it does not mount APFS.

Host tests: python3 ../tools/guest-daemon-test/run.py.
See ../PHONE-CACHE.md for the phone failure and upgrade instructions.

Synthetic Darwin runtime probe, with caller-owned boot inputs and NEW scratch
work/output paths only (never use a restored iOS disk):

```sh
CLANG=/path/to/clang MACHO_LD=/path/to/ld64.lld ./guest/build.sh \
  /scratch/probe-daemon ./tools/guest-daemon-test/probe.c
JAVA_HOME=/path/to/jdk GRADLE_USER_HOME=/path/to/gradle-cache \
  tools/patch-ramdisk-probe/build-with-tools.sh /owned/stock.im4p \
  /scratch/probe-injected.im4p /scratch/probe-patch.im4p /scratch/probe-daemon
python3 tools/patch-boot-probe/run.py --qemu /path/to/linux/qemu-system-aarch64 \
  --inputs /owned/inputs --kernel /owned/kernelcache.research.iphone12b \
  --ramdisk /scratch/probe-patch.im4p --work /scratch/NEW-work \
  --expect-daemon-self-test --expect-native-unmount
```

Run these commands from InfernoAndroid/ after compiling the Kotlin classes.
The expected marker is `EMBER DAEMON SYNTHETIC GUEST RESULT first=0 second=0`.
Fake mount/patcher commands are unchanged copies of the signed probe Mach-O;
there is no dependency on a guest shell. The probe also mounts and unmounts a
real temporary tmpfs at /mnt9 to exercise the Darwin syscall without a fake umount tool. Real APFS/FS-patcher/iOS boot are not
covered. Only the normal production daemon belongs in the APK.

0.3.0 adds a reviewed replacement-byte map (45 sites, build 18A5351d only).
Regenerate cache-plan.h with python3 plans/generate.py. Complete matching sites
can be reused without running the C++ patcher; incomplete/failed readback is not
success. A normal C++ exit 0 also needs matching readback. This is not a hash
of the entire cache. The synthetic guest probe explicitly uses a different
2-site plan, which the Android monitor rejects as production evidence.
Guest sync/unmount cleanup finishes before the host reacts to a child failure.

0.3.1 patches five exact service labels in /System/Library/xpc/launchd.plist
on the already mounted System volume; it does NOT mount Data. The bounded
portable bplist00 editor in launchd-cache.c preserves unrelated objects and
shared false booleans, appends replacement dictionaries, verifies the result
and retains launchd.plist.ember-original before atomic publication. XML/unknown
formats or missing/ambiguous labels fail safely. Normal-boot Data unlock is
not proven by this change. See ../PHONE-DATA.md.

Run independent binary-plist tests:

```sh
python3 tools/launchd-cache-test/run.py
EMBER_SANITIZE_FLAGS='-fsanitize=address,undefined -fno-pie -no-pie' \
  python3 tools/launchd-cache-test/run.py
python3 tools/launchd-cache-test/make-fixture.py
```

These commands run from InfernoAndroid/. make-fixture.py regenerates the
synthetic C fixture for the guest probe; it contains no Apple file data.
