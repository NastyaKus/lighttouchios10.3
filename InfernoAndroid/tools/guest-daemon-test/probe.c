// SPDX-License-Identifier: GPL-3.0-or-later
// Guest runtime integration test of the production daemon's Darwin ABI.
// Every "volume"/command is fake and lives in RAM under /ember-daemon-probe. No APFS writes.
#define EMBER_SYNTHETIC_PATCH_PLAN
#define EMBER_TEST_ROOT "/ember-daemon-probe"
#define unmount synthetic_unmount
#define main daemon_main
#include "../../guest/patcher-daemon.c"
#undef main
#undef unmount
#include "launchd-fixture.h"
extern int unmount(const char *, int);
int synthetic_unmount(const char *path, int flags) { (void)path;return flags == 0 ? 0 : -1; }
static int put(const char *path, const char *contents, unsigned long bytes, int executable) {
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, executable ? 0755 : 0644);
    if (fd < 0) { dprintf(1,"EMBER PROBE: cannot open %s errno=%d\n",path,errno); return 1; }
    int ok = write(fd, contents, bytes) == (ssize_t)bytes;
    close(fd);return !ok;
}
#define PUT(path, body, executable) put(path, body, sizeof(body)-1, executable)
static int copy_self(const char *path) {
    int input = open("/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-01.30-006-P.bin",O_RDONLY);
    int output = open(path,O_WRONLY|O_CREAT|O_TRUNC,0755);
    if(input < 0 || output < 0) return 1;
    char bytes[4096];ssize_t n;
    while((n=read(input,bytes,sizeof(bytes))) > 0) if(write(output,bytes,(size_t)n) != n) return 1;
    close(input);close(output);return n < 0;
}
int main(int argc, char **argv) {
    // Each fake command is this same signed Mach-O, copied unchanged. Restore
    // ramdisk has no /bin/sh, so #!/bin/sh mocks cannot execute here.
    if(argc > 0 && strncmp(argv[0],ROOT "/sbin/",sizeof(ROOT "/sbin/")-1)==0) return 0;
    if(argc > 0 && strncmp(argv[0],PATCHER,sizeof(PATCHER))==0) {
        if(argc != 2 || strncmp(argv[1],CACHE,sizeof(CACHE))!=0) return 92;
        int fd=open(CACHE,O_RDWR);if(fd<0)return 96;
        for(unsigned i=0;i<EMBER_PLAN_COUNT;i++) {
            if(lseek(fd,expected_patches[i].offset,0)!=expected_patches[i].offset || write(fd,expected_patches[i].bytes,4)!=4)return 97;
        }
        close(fd);dprintf(1,"synthetic-cache-test-success\n");return 0;
    }
    dprintf(1,"EMBER PROBE: beginning synthetic guest test\n");
    char *rw[] = {"/sbin/mount", "-u", "-w", "/", NULL};
    dprintf(1,"EMBER PROBE: remount root says %d\n",run(rw[0],rw));
    const char *dirs[] = {ROOT, ROOT "/dev", ROOT "/sbin", ROOT "/mnt9",
        ROOT "/mnt9/System", ROOT "/mnt9/System/Library", ROOT "/mnt9/System/Library/Caches",
        ROOT "/mnt9/System/Library/Caches/com.apple.dyld", ROOT "/mnt9/System/Library/xpc", ROOT "/mnt8", ROOT "/usr",
        ROOT "/usr/standalone", ROOT "/usr/standalone/firmware", ROOT "/usr/standalone/firmware/nfrestore",
        ROOT "/usr/standalone/firmware/nfrestore/firmware", ROOT "/usr/standalone/firmware/nfrestore/firmware/jcop-prod"};
    for (unsigned i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) if(mkdir(dirs[i],0755) != 0) dprintf(1,"EMBER PROBE: mkdir %s errno=%d\n",dirs[i],errno);
    // Exercise the REAL Darwin mount/unmount implementation as well, not just
    // mocked coordinator operations. Stock mount_tmpfs has the mount privilege;
    // this unprivileged daemon still makes the actual unmount syscall. No APFS.
    mkdir("/mnt9",0755);
    char *tmpfs[] = {"/sbin/mount_tmpfs", "-s", "8m", "/mnt9", NULL};
    int mounted = run(tmpfs[0],tmpfs);
    errno = 0;
    int detached = mounted == 0 ? unmount("/mnt9",0) : -1;
    int detach_errno = errno;
    errno = 0;
    int missing = unmount(ROOT "/not-a-mount",0), missing_errno = errno;
    dprintf(1,"EMBER NATIVE UNMOUNT RESULT mount=%d unmount=%d errno=%d missing=%d missing_errno=%d\n",mounted,detached,detach_errno,missing,missing_errno);
    if(mounted != 0 || detached != 0 || missing != -1 || missing_errno == 0) { dprintf(1,"EMBER NATIVE UNMOUNT TEST FAILED\n"); return 94; }
    if (put(ROOT "/mnt9/System/Library/xpc/launchd.plist",(const char *)launchd_fixture,sizeof(launchd_fixture),0) || PUT(SYSTEM_VOLUME,"fake device",0) || PUT(DATA_VOLUME,"fake device",0) ||
        copy_self(ROOT "/sbin/mount_apfs") || copy_self(PATCHER) ||
        PUT(CACHE ".InfernoOriginalBytes","8143964: 1f 20 03 d5\n",0) ||
        PUT(CACHE,"dyld_v1  arm64e\0\0",0)) return 91;
    int fd = open(CACHE,O_WRONLY);
    if (fd < 0 || ftruncate(fd,17*1024*1024) != 0) return 93;
    close(fd);
    int first = daemon_main(), second = daemon_main();
    dprintf(1,"EMBER DAEMON SYNTHETIC GUEST RESULT first=%d second=%d\n",first,second);
    return first || second;
}
