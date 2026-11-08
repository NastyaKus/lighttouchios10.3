// SPDX-License-Identifier: GPL-3.0-or-later
// Linux link-time replacement ONLY for tests. No host mount is ever touched.
#include <errno.h>
#include <stdlib.h>
#include <string.h>
int __wrap_umount2(const char *path, int flags) {
    if(flags != 0) { errno = EINVAL; return -1; }
    const char *fail = getenv("EMBER_TEST_UNMOUNT_FAILURE");
    if(fail && (strcmp(fail,"all")==0 || (strcmp(fail,"data")==0 && strstr(path,"/mnt8")))) {
        errno = EBUSY;return -1;
    }
    return 0;
}

void __wrap_sync(void) { /* no global host sync in tests */ }
