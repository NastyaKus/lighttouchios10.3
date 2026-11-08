/* SPDX-License-Identifier: GPL-3.0-or-later
 * Disposable COW System only. Caller MUST enforce immutable stock backing
 * and a bounded throw-away overlay. Never used for an installed kit. */
#include "../../guest/darwin-abi.h"
extern int chroot(const char *),chdir(const char *);
static int copy(const char *source,const char *target) {
    int in=open(source,O_RDONLY);if(in<0)return 0;
    int out=open(target,O_CREAT|O_TRUNC|O_WRONLY,0755);if(out<0){close(in);return 0;}
    unsigned char buffer[8192];int ok=1;
    for(;;) {
        ssize_t n=read(in,buffer,sizeof(buffer));if(n<0&&errno==EINTR)continue;if(n<0){ok=0;break;}if(!n)break;
        ssize_t at=0;while(at<n){ssize_t wrote=write(out,buffer+at,(size_t)(n-at));if(wrote<0&&errno==EINTR)continue;if(wrote<=0){ok=0;break;}at+=wrote;}
        if(!ok)break;
    }
    if(fsync(out))ok=0;close(in);if(close(out))ok=0;return ok;
}
int ember_stock_stage_chroot(void) {
    mkdir("/mnt9/usr/lib/ember",0755);
    if(!copy(
#ifdef EMBER_SYSTEM_EXECUTABLE
            "/usr/libexec/backboardd","/mnt9/usr/libexec/backboardd"
#else
            "/usr/lib/ember/probe","/mnt9/usr/lib/ember/probe"
#endif
        ) ||
       !copy("/usr/lib/ember/libEmberSystemGLES.dylib","/mnt9/usr/lib/ember/libEmberSystemGLES.dylib")) {
        dprintf(1,"*** EMBER-STOCK: FAIL stage_chroot errno=%d\n",errno);return 0;
    }
    dprintf(1,"*** EMBER-STOCK: staged_test_files disposable_overlay=1\n");return 1;
}
int ember_stock_enter_chroot(void) {
    if(chroot("/mnt9") || chdir("/"))return 0;
    return 1;
}
