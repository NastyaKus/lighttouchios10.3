/* SPDX-License-Identifier: GPL-3.0-or-later */
int emberLoaderProbe(void) { return 14; }
#ifdef EMBER_BACKBOARDD_CONSTRUCTOR_TEST
#include "../../guest/darwin-abi.h"
#include "../../gpu-bridge/system/eagl-native.h"
extern int getpid(void),getuid(void);
extern void *dlsym(void *,const char *);
extern const unsigned _mh_dylib_header[];
__attribute__((constructor)) static void native_dependency_loaded(void) {
    const char *(*program)(void)=(const char*(*)(void))dlsym((void *)(long)-2,"getprogname");
    if(_mh_dylib_header[2]!=0x80000002u || !program || strncmp(program(),"backboardd",64) || !egNativeEaglInstall()) {
        dprintf(1,"*** EMBER-BB-LOADER: FAIL native_constructor %s\n",egNativeEaglError());return;
    }
    dprintf(1,"*** EMBER-BB-LOADER: native_dependency_loaded program=backboardd dylib_cpusubtype=0x80000002 pid=%d uid=%d forwarding_only=1\n",getpid(),getuid());
}
#endif
