/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "broker-client-probe.h"
#include "gles2-subset.h"
#include "stream.h"
static const char *socket_path,*forbidden;
int egProbeConfigure(int argc,char **argv) {
    if(argc!=5 || strncmp(argv[1],"--broker",9) || strncmp(argv[3],"--forbidden",12))return 0;
    socket_path=argv[2];forbidden=argv[4];return 1;
}
const char *egBrokerSocket(void) { return socket_path; }
int egBrokerClientIdentity(void) {
#ifdef EGPU_GUEST_FREESTANDING
    if(getuid()!=501 || geteuid()!=501 || getgid()!=501 || getegid()!=501) {
        dprintf(1,"*** EGPU-BROKER-CLIENT: FAIL uid/gid must be 501\n");return 0;
    }
    if(strncmp(forbidden,"/dev/rdisk",10))return 0;
    int rd=open(forbidden,O_RDONLY),rd_error=errno;if(rd>=0)close(rd);
    int wr=open(forbidden,O_RDWR),wr_error=errno;if(wr>=0)close(wr);
    if(rd>=0 || wr>=0 || (rd_error!=1 && rd_error!=13) || (wr_error!=1 && wr_error!=13)) {
        dprintf(1,"*** EGPU-BROKER-CLIENT: FAIL raw denial rd=%d errno=%d wr=%d errno=%d\n",rd,rd_error,wr,wr_error);return 0;
    }
    dprintf(1,"*** EGPU-BROKER-CLIENT: uid=501 gid=501 raw_read_denied=%d raw_write_denied=%d\n",rd_error,wr_error);
#else
    dprintf(1,"*** EGPU-BROKER-CLIENT: Linux IPC fixture uid=%u; no iOS privilege claim\n",(unsigned)geteuid());
#endif
    return 1;
}
#ifndef EGPU_GUEST_FREESTANDING
EgGlesClient *egProbeOpen(const char *unused,uint32_t w,uint32_t h) {
    (void)unused;if(!egBrokerClientIdentity())return NULL;
    return egGlesOpenBroker(socket_path,3,w,h);
}
#endif
