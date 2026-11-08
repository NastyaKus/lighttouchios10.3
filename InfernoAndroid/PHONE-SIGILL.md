# Android native crash during USB bring-up

The on-device 0.2.1 report establishes:

- Android recorded NATIVE_CRASH with status 4 (SIGILL) for the :vm process.
- The worker began and the process died roughly 10 seconds later.
- The last saved phase was Waiting for guest USB; there was no host-side
  usbmux-ready record, restored QueryType or ASR transfer.
- The guest console reached launchd/restored_external and USB stack bring-up.
  This verifies execution of the restore ramdisk, not an installed iOS disk.
- Android did not attribute this termination to LOW_MEMORY. Available memory
  measured after the process had died does not establish its peak usage.
- The supplied 0.2.1 archive lacks a native instruction address/backtrace.
  The faulting library and function remain unknown. SIGILL alone is not
  sufficient to blame CPU capabilities, the ticket, RAM, or a USB function.

0.2.2 is a diagnostic update, not a claimed fix for SIGILL. It preserves the
native engine and JNI binaries and adds:

1. Export of Android's own native :vm tombstones via
   ApplicationExitInfo.getTraceInputStream(), if Android retains them.
   Only native VM crashes from this package are selected (up to three,
   capped at 4 MiB per trace); availability/errors are recorded separately.
2. First USB request/response metadata (IDs, endpoints, status and lengths),
   bounded during enumeration. Firmware/ticket payloads are not logged.

Install over the existing app and export diagnostics before starting another
restore. A tombstone may be absent/expired. If absent, the new early-USB trace
will help identify the last exchange on a later controlled retry. No inputs
need to be downloaded or prepared again for this diagnostic export.


## Native tombstone obtained with 0.2.2

The new report contains a complete native tombstone. The crashing thread is
inferno.qemu. Its relevant chain is:

```
libc.so longjmp+460             (autiasp, d50323bf)
qemu_coroutine_switch+104
qio_channel_readv_full_all_eof
usb_inferno_host_msg_loop_co
coroutine_bootstrap
coroutine_trampoline
```

The USB trace ends after RESET and the first SETUP request. This identifies the
bionic saved-return-address authentication path during QEMU coroutine switching.
The original registers/checksum progressed to the final autiasp, which faulted.
No private user tombstone, full register dump, guest log or device identifiers
are included in the public source archive.

0.2.3 adds a private Android AArch64 context primitive adapted from the Android
Inferno fork rather than changing Android-wide pointer authentication. The
pinned core and all boot/restore protocol logic remain, with one additional
patch. The exact switch disassembly calls qemu_android_fiber_setjmp/longjmp,
not bionic sigsetjmp/siglongjmp. The native regression passed on cortex-a57
and max with branch-protected C functions and 100000 separate-stack switches.
This targets the observed native failure. The full restore is still not claimed
until the user tests the updated APK on the actual Android device.
