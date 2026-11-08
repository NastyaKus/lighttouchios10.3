// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from MakrSas/Inferno-iOS @ 4bc3970 app/guest/patcher-daemon.c.
// Patch-only boot: restored_external MUST stay disabled. No formatting/restore.
#ifdef __APPLE__
#include "darwin-abi.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "cache-plan.h"
#include "launchd-cache.h"
#ifndef EMBER_TEST_ROOT
#define EMBER_TEST_ROOT ""
#endif
#define ROOT EMBER_TEST_ROOT
#define SYSTEM_VOLUME ROOT "/dev/disk0s1s1"
#define DATA_VOLUME ROOT "/dev/disk0s1s2"
#define SYSTEM_MOUNT ROOT "/mnt9"
#define DATA_MOUNT ROOT "/mnt8"
// Preparation accepts ONLY 18A5351d/n104ap. Never scan by prefix: backups and
// .map files have the same prefix, and readdir order changes between boots.
#define CACHE SYSTEM_MOUNT "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e"
#define PATCHER ROOT "/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-11.04-012.2-P.bin"
#define LOG SYSTEM_MOUNT "/inferno_patch.log"
#define OVERRIDES DATA_MOUNT "/db/com.apple.xpc.launchd"
#define NOTE(...) dprintf(1, "*** PATCHER: " __VA_ARGS__)

static int run_to(const char *out, const char *path, char *const args[]) {
    pid_t child = fork();
    if (child < 0) return 125;
    if (child == 0) {
        if (out != NULL) {
            int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) _exit(126);
            if (fd > 2) close(fd);
        }
        execv(path, args);
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child) return 125;
    // A signal MUST NOT look like exit 0 (AMFI/low memory can kill a child).
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
}
static int run(const char *path, char *const args[]) { return run_to(NULL, path, args); }

static int mount_volume(const char *volume, const char *point) {
    mkdir(point, 0755);
    NOTE("mounting %s on %s\n", volume, point);
    char *args[] = {ROOT "/sbin/mount_apfs", "-o", "rw", (char *)volume, (char *)point, NULL};
    int code = 1;
    for (int tries = 0; tries < 100 && code != 0; tries++) {
        code = run(args[0], args);
        if (code != 0) usleep(200000);
    }
    NOTE("mount_apfs %s says %d\n", volume, code);
    return code;
}
static int unmount_volume(const char *point) {
    NOTE("sync before unmount %s\n", point);
    sync();
    NOTE("unmounting %s\n", point);
    // This restore ramdisk has mount_apfs but NO umount executable. Use the
    // public Darwin syscall, without MNT_FORCE: an EBUSY/error must preserve
    // the attempt and prevent activation, never be hidden by a forced detach.
    errno = 0;
#ifdef __APPLE__
    int code = unmount(point, 0);
#else
    int code = umount2(point, 0); // Linux host tests wrap this; never detach host mounts.
#endif
    int saved = errno;
    NOTE("unmount %s says %d errno=%d\n", point, code, saved);
    if (code != 0) NOTE("ERROR: unmount %s failed errno=%d\n", point, saved);
    return code;
}
static int cache_valid(off_t *size) {
    int fd = open(CACHE, O_RDONLY);
    if (fd < 0) return 0;
    char magic[17] = {0};
    ssize_t got = read(fd, magic, 16);
    *size = lseek(fd, 0, SEEK_END);
    close(fd);
    return got == 16 && *size >= 16 * 1024 * 1024 &&
        strncmp(magic, "dyld_v1", 7) == 0 && strstr(magic, "arm64e") != NULL;
}
// Read-only verification. A missing/changed site never counts as success.
static int verify_plan(off_t size) {
    int fd=open(CACHE,O_RDONLY);if(fd<0)return 0;
    unsigned matched=0;
    for(unsigned i=0;i<EMBER_PLAN_COUNT;i++) {
        const struct CachePatch *p=&expected_patches[i];unsigned char got[4];
        if(p->offset<0 || p->offset>size-4 || lseek(fd,p->offset,0)!=p->offset || read(fd,got,4)!=4)continue;
        if(got[0]==p->bytes[0] && got[1]==p->bytes[1] && got[2]==p->bytes[2] && got[3]==p->bytes[3])matched++;
    }
    close(fd);
    NOTE("patch plan check %s matched=%u total=%u\n",EMBER_PLAN_ID,matched,(unsigned)EMBER_PLAN_COUNT);
    if(matched!=EMBER_PLAN_COUNT)return 0;
    NOTE("patch plan verified %s all=%u\n",EMBER_PLAN_ID,(unsigned)EMBER_PLAN_COUNT);
    return 1;
}
static int durable_cache(void) {
    int fd=open(CACHE,O_RDWR);if(fd<0)return 1;
    int code=fsync(fd);close(fd);if(code!=0)return 1;
    // If the upstream patcher recreated its original-byte file, make its data
    // durable too. sync + unmount below also commits directory metadata.
    fd=open(CACHE ".InfernoOriginalBytes",O_RDONLY);
    if(fd>=0) { code=fsync(fd);close(fd);if(code!=0)return 1; }
    return 0;
}
#define LAUNCHD SYSTEM_MOUNT "/System/Library/xpc/launchd.plist"
static int read_file(const char *path,unsigned char **bytes,size_t *length) {
    *bytes=NULL;*length=0;int fd=open(path,O_RDONLY);if(fd<0)return 1;
    off_t n=lseek(fd,0,SEEK_END);
    if(n<8 || n>32*1024*1024 || lseek(fd,0,0)!=0){close(fd);return 1;}
    unsigned char *buf=malloc((size_t)n);if(!buf){close(fd);return 1;}
    size_t at=0;
    while(at<(size_t)n){ssize_t got=read(fd,buf+at,(size_t)n-at);if(got<0&&errno==EINTR)continue;if(got<=0){free(buf);close(fd);return 1;}at+=(size_t)got;}
    close(fd);*bytes=buf;*length=(size_t)n;return 0;
}
static int write_file(const char *path,const unsigned char *bytes,size_t length) {
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd<0)return 1;size_t at=0;
    while(at<length){ssize_t n=write(fd,bytes+at,length-at);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return 1;}at+=(size_t)n;}
    int result=fsync(fd);if(close(fd)!=0)result=1;return result!=0;
}
static int disable_services(void) {
    NOTE("patching System launchd cache; Data mount is not required\n");
    unsigned char *in=NULL,*out=NULL,*check=NULL;size_t n=0,outn=0,checkn=0;
    const char *why="I/O failure";int result=1;
    if(read_file(LAUNCHD,&in,&n)!=0){NOTE("launchd cache read failed errno=%d\n",errno);goto done;}
    if(!lc_patch(in,n,&out,&outn,&why)){NOTE("launchd cache validation failed: %s\n",why);goto done;}
    if(n!=outn || memcmp(in,out,n)!=0) {
        // Retain the first original. Publish backup before touching live cache.
        if(access(LAUNCHD ".ember-original",F_OK)!=0) {
            if(errno!=2 || write_file(LAUNCHD ".ember-original.part",in,n)!=0 ||
               rename(LAUNCHD ".ember-original.part",LAUNCHD ".ember-original")!=0){NOTE("launchd cache backup failed errno=%d\n",errno);goto done;}
        }
        if(write_file(LAUNCHD ".ember-part",out,outn)!=0 || read_file(LAUNCHD ".ember-part",&check,&checkn)!=0 ||
           !lc_verify(check,checkn,&why)){NOTE("launchd cache temporary write/readback failed: %s errno=%d\n",why,errno);goto done;}
        free(check);check=NULL;
        if(rename(LAUNCHD ".ember-part",LAUNCHD)!=0){NOTE("launchd cache atomic rename failed errno=%d\n",errno);goto done;}
    }
    if(read_file(LAUNCHD,&check,&checkn)!=0 || !lc_verify(check,checkn,&why)){NOTE("launchd cache final readback failed: %s\n",why);goto done;}
    NOTE("launchd cache verified: five exact labels Disabled=true\n");
    NOTE("five services switched off\n");result=0;
done:
    free(in);free(out);free(check);return result;
}
int main(void) {
    int console=open(ROOT "/dev/console",O_WRONLY);
    if(console>=0) { dup2(console,1);dup2(console,2);if(console>2)close(console); }
    NOTE("Ember daemon 0.3.1, System service cache, safe cleanup\n");
    NOTE("waiting for the restore to finish\n");
    const char *failure="patch-only boot incomplete; keep disks and export diagnostics";
    while(access(SYSTEM_VOLUME,F_OK)!=0)sleep(2);
    NOTE("the system volume is here -- patch-only boot\n");
    if(mount_volume(SYSTEM_VOLUME,SYSTEM_MOUNT)!=0)goto fail;
    off_t size=0;
    if(!cache_valid(&size)) {
        failure="expected arm64e dyld cache missing, too small or invalid";
        unmount_volume(SYSTEM_MOUNT);goto fail;
    }
    int code=0;
    if(verify_plan(size)) {
        NOTE("existing complete patch set reused; cache not rewritten\n");
    } else {
        NOTE("patching %s, %lld MB\n",CACHE,(long long)size>>20);
        char *args[]={PATCHER,CACHE,NULL};
        code=run_to(LOG,PATCHER,args);
        NOTE("the patcher exited with %d\n",code);
        int log=open(LOG,O_RDONLY);
        if(log>=0) {
            static char text[65536];ssize_t got=read(log,text,sizeof(text)-1);close(log);
            if(got>0) { text[got]=0;NOTE("what it wrote ---\n%s\n*** PATCHER: --- end\n",text); }
        }
        if(code==0 && !verify_plan(size)) { code=1;failure="patcher exit 0 but expected patch plan is incomplete"; }
    }
    if(code==0 && durable_cache()!=0) { code=1;failure="cache/original-byte fsync failed"; }
    // The host MUST wait here even after a nonzero child exit. Do not publish
    // cleanup-complete until the guest has finished sync/unmount processing.
    if(code==0 && disable_services()!=0){code=1;failure="System launchd service cache update failed";}
    if(unmount_volume(SYSTEM_MOUNT)!=0 || code!=0)goto fail;
    NOTE("cleanup complete\n");
    NOTE("done, the machine can be reset now\n");return 0;
fail:
    NOTE("cleanup complete\n");
    NOTE("ERROR: %s\n",failure);return 1;
}
