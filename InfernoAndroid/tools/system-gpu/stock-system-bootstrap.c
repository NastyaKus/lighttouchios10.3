/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test environment only: locate the exact read-only stock System GPT by hash,
 * mount it read-only, request a private dyld cache for our next exec. No APFS
 * writes, existing guest changes, kernel patches or shared-region replacement. */
#include <stdint.h>
#include "../../guest/darwin-abi.h"
extern int snprintf(char *,size_t,const char *,...);
extern int setenv(const char *,const char *,int);
extern unsigned char *CC_SHA256(const void *,unsigned int,unsigned char *);
static const uint8_t expected[32]={0x20,0x1f,0xb0,0x77,0xe0,0x01,0x2a,0xb6,0xb5,0x29,0x05,0xb5,0x77,0xd5,0x71,0x3d,
    0x5c,0x52,0x8f,0x26,0x5b,0xf3,0x7c,0x7a,0xd3,0x9c,0x11,0xba,0x24,0xdc,0xdb,0x9a};
int ember_stock_setup(int (*run)(char *const [])) {
    char path[64];int disk=-1;uint8_t bytes[8192],hash[32];
    for(int attempt=0;attempt<40 && disk<0;attempt++) {
        for(int i=1;i<=8;i++) {
            snprintf(path,sizeof(path),"/dev/rdisk%d",i);int fd=open(path,O_RDONLY);if(fd<0)continue;
            size_t at=0;while(at<sizeof(bytes)) { ssize_t n=read(fd,bytes+at,sizeof(bytes)-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=(size_t)n; }
            close(fd);if(at!=sizeof(bytes))continue;CC_SHA256(bytes,sizeof(bytes),hash);
            if(!memcmp(hash,expected,32)){disk=i;break;}
        }
        if(disk<0)usleep(500000);
    }
    if(disk<0){dprintf(1,"*** EMBER-STOCK: FAIL exact_GPT_not_found\n");return 0;}
    dprintf(1,"*** EMBER-STOCK: exact_GPT_verified disk=%d\n",disk);
    // Use the restore environment's established System mount point, matching
    // the working patch-only daemon. A custom /mnt/ember-stock was denied by
    // mount_apfs's System Policy even with a read-only backing device.
    mkdir("/mnt9",0755);
    int result=-1;
    for(int choice=0;choice<2;choice++) {
        snprintf(path,sizeof(path),choice?"/dev/disk%ds1":"/dev/disk%ds1s1",disk);
        if(access(path,F_OK))continue;
        char *args[]={"/sbin/mount_apfs","-o",
#ifdef EMBER_STOCK_CHROOT
            "rw",
#else
            "ro",
#endif
            path,"/mnt9",NULL};
        result=run(args);dprintf(1,"*** EMBER-STOCK: mount=%d mode=%s source=%s\n",result,args[2],path);
        if(!result)break;
    }
    if(result){dprintf(1,"*** EMBER-STOCK: FAIL readonly_mount=%d\n",result);return 0;}
    char *mounts[]={"/sbin/mount",NULL};run(mounts);
#ifndef EMBER_STOCK_CHROOT
    if(setenv("DYLD_SHARED_REGION","private",1) ||
       setenv("DYLD_SHARED_CACHE_DIR","/mnt9/System/Library/Caches/com.apple.dyld",1) ||
       setenv("DYLD_ROOT_PATH","/mnt9",1))return 0;
    dprintf(1,"*** EMBER-STOCK: private_cache_requested next_exec=1\n");return 1;
#else
    return 1;
#endif
}
