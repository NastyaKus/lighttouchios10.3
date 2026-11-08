/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only APFS extraction using the pinned Inferno block/APFS implementation.
 * No mounting, firmware modification or installed-device writes. */
#include "qemu/osdep.h"
#include "block/block.h"
#include "crypto/init.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/block-backend.h"
typedef struct ApfsFile ApfsFile;
extern ApfsFile *apfs_open_file(BlockBackend *,uint16_t,const char *,Error **);
extern ApfsFile *ember_apfs_open_container(BlockBackend *,const char *,Error **);
extern uint64_t apfs_file_size(const ApfsFile *);
extern bool apfs_file_pread(ApfsFile *,uint64_t,uint64_t,void *,Error **);
extern void apfs_file_free(ApfsFile *);
int main(int argc,char **argv) {
    if(argc!=4){fprintf(stderr,"usage: read-system-file QEMU-readonly-source APFS-path NEW-output\n");return 2;}
    qemu_init_main_loop(&error_fatal);qcrypto_init(&error_fatal);module_call_init(MODULE_INIT_QOM);bdrv_init();
    Error *error=NULL;BlockBackend *disk=blk_new_open(argv[1],NULL,NULL,0,&error);
    if(!disk){fprintf(stderr,"open: %s\n",error_get_pretty(error));error_free(error);return 1;}
    if(!strcmp(argv[2],"@header")) {
        unsigned char header[8192];int fd=open(argv[3],O_CREAT|O_EXCL|O_WRONLY,0600);
        int ok=fd>=0 && blk_pread(disk,0,sizeof(header),header,0)>=0 && write(fd,header,sizeof(header))==sizeof(header);
        if(fd>=0)close(fd);blk_unref(disk);return ok?0:1;
    }
    ApfsFile *file=ember_apfs_open_container(disk,argv[2],&error);
    if(!file){fprintf(stderr,"APFS: %s\n",error_get_pretty(error));error_free(error);blk_unref(disk);return 1;}
    uint64_t size=apfs_file_size(file);int fd=open(argv[3],O_CREAT|O_EXCL|O_WRONLY,0600),status=1;
    if(fd<0){perror("create output");goto done;}
    unsigned char *bytes=malloc(1<<20);if(!bytes){close(fd);goto done;}
    for(uint64_t at=0;at<size;) {
        size_t n=MIN(size-at,1<<20);
        if(!apfs_file_pread(file,at,n,bytes,&error)){fprintf(stderr,"read offset=%" PRIu64 ": %s\n",at,error_get_pretty(error));break;}
        size_t wrote=0;while(wrote<n){ssize_t k=write(fd,bytes+wrote,n-wrote);if(k<0&&errno==EINTR)continue;if(k<=0)break;wrote+=k;}
        if(wrote!=n){perror("write output");break;}
        at+=n;if(!(at% (64<<20)) || at==size)fprintf(stderr,"read %" PRIu64 "/%" PRIu64 " bytes\n",at,size);
        if(at==size)status=0;
    }
    free(bytes);if(fsync(fd) || close(fd))status=1;
done:
    apfs_file_free(file);blk_unref(disk);return status;
}
