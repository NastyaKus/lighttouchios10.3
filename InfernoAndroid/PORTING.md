# Ember VM restore port

Android installer code is derived from MakrSas/Inferno-iOS @ 4bc39702871f5a118d96acbad4df7a675ddf14e0 (GPL-3.0). New restore code: GPL-3.0-or-later. The engine retains its own AGPL/GPL notices and pinned source revision; Android compatibility patches are under engine/patches. Version 0.2.3 adds the narrowly scoped AArch64 coroutine-context fix derived from AlexZorzi/Inferno-Android. This directory is the scope of the port; the iPhone 5 archive is untouched.

App name: Ember VM. Application ID / namespace: dev.embervm.android. Existing Inferno installations and their data stay separate.

0.2.8 replaces only the guest patch coordinator with the GPL C source in guest/. Exact 18A5351d cache selection excludes .InfernoOriginalBytes; patch-only boots update already prepared ramdisks. QEMU/JNI and the upstream filesystem patcher remain unchanged. See PHONE-CACHE.md.

0.2.9 removes the missing /sbin/umount dependency via native Darwin unmount(path, 0), with errno diagnostics and no forced detach. See PHONE-UNMOUNT.md.

0.3.0 verifies the pinned 45-site replacement plan before reusing an already patched cache and after a normal patcher exit. Host waits for guest cleanup before child-failure shutdown. See PHONE-REPEAT.md; full cache integrity/APFS/iOS boot remain separate checks.

0.3.1 edits only five Disabled flags in the System launchd cache with backup/atomic readback. The restore-ramdisk Data mount is removed; Data unlock at normal boot remains unverified. Portable bounded bplist C editor and independent plistlib/sanitizer tests are included. See PHONE-DATA.md.
