/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "stream.h"
#ifdef EGPU_GUEST_FREESTANDING
#define EG_SOCKADDR(p) (p)
#else
/* bionic declares connect/bind without glibc's transparent sockaddr union. */
#define EG_SOCKADDR(p) ((const struct sockaddr *)(p))
#endif
uint64_t eg_stream_now(void) {
    struct eg_timespec ts;
    if(clock_gettime(EG_CLOCK_MONOTONIC,&ts))return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
int eg_stream_wait(int fd,short events,uint64_t deadline) {
    for(;;) {
        uint64_t now=eg_stream_now();if(!now || now>=deadline)return 0;
        struct eg_pollfd p={fd,events,0};int wait=(deadline-now)>1000?1000:(int)(deadline-now);
        int result=poll(&p,1,wait);
        if(result>0)return (p.revents&events)!=0;
        if(result<0 && errno!=EINTR)return 0;
    }
}
int eg_stream_read(int fd,void *data,uint32_t bytes,uint64_t deadline) {
    uint8_t *p=data;uint32_t at=0;
    while(at<bytes) {
        if(!eg_stream_wait(fd,EG_POLLIN,deadline))return 0;
        ssize_t n=read(fd,p+at,bytes-at);
        if(n>0)at+=(uint32_t)n;else if(n==0 || (errno!=EINTR && errno!=EG_EAGAIN))return 0;
    }
    return 1;
}
int eg_stream_write(int fd,const void *data,uint32_t bytes,uint64_t deadline) {
    const uint8_t *p=data;uint32_t at=0;
    while(at<bytes) {
        if(!eg_stream_wait(fd,EG_POLLOUT,deadline))return 0;
#ifdef EGPU_GUEST_FREESTANDING
        ssize_t n=send(fd,p+at,bytes-at,0);
#else
        ssize_t n=send(fd,p+at,bytes-at,MSG_NOSIGNAL);
#endif
        if(n>0)at+=(uint32_t)n;else if(n==0 || (errno!=EINTR && errno!=EG_EAGAIN))return 0;
    }
    return 1;
}
int eg_stream_cloexec(int fd,int enabled) {
    int flags=fcntl(fd,EG_F_GETFD);if(flags<0)return 0;
    return !fcntl(fd,EG_F_SETFD,enabled?(flags|EG_FD_CLOEXEC):(flags&~EG_FD_CLOEXEC));
}
static int address(struct eg_sockaddr_un *addr,const char *name) {
    size_t n=0;while(n<sizeof(addr->sun_path) && name[n])n++;
    if(!n || n>=sizeof(addr->sun_path))return 0;
    memset(addr,0,sizeof(*addr));memcpy(addr->sun_path,name,n+1);
#ifdef EGPU_GUEST_FREESTANDING
    addr->sun_len=(uint8_t)sizeof(*addr);addr->sun_family=EG_AF_UNIX;
#else
    addr->sun_family=EG_AF_UNIX;
#endif
    return 1;
}
int eg_stream_prepare(int fd) {
    if(!eg_stream_cloexec(fd,1))return 0;
    int flags=fcntl(fd,EG_F_GETFL);
    if(flags<0 || fcntl(fd,EG_F_SETFL,flags|EG_O_NONBLOCK))return 0;
#ifdef EGPU_GUEST_FREESTANDING
    int yes=1;if(setsockopt(fd,EG_SOL_SOCKET,EG_SO_NOSIGPIPE,&yes,sizeof(yes)))return 0;
#endif
    return 1;
}
static int new_socket(void) {
    int fd=socket(EG_AF_UNIX,EG_SOCK_STREAM,0);if(fd<0)return -1;
    if(!eg_stream_prepare(fd)){close(fd);return -1;}
    return fd;
}
int eg_stream_connect(const char *name) {
    struct eg_sockaddr_un addr;if(!address(&addr,name))return -1;
    int fd=new_socket();if(fd<0)return -1;
    if(connect(fd,EG_SOCKADDR(&addr),sizeof(addr))) {
        int error=0;eg_socklen_t size=sizeof(error);
        if(errno!=EG_EINPROGRESS || !eg_stream_wait(fd,EG_POLLOUT,eg_stream_now()+5000) ||
           getsockopt(fd,EG_SOL_SOCKET,EG_SO_ERROR,&error,&size) || error){close(fd);return -1;}
    }
    return fd;
}
int eg_stream_listen(const char *name) {
    struct eg_sockaddr_un addr;if(!address(&addr,name))return -1;
    int fd=new_socket();if(fd<0)return -1;
    if(bind(fd,EG_SOCKADDR(&addr),sizeof(addr))){close(fd);return -1;}
    if(listen(fd,4)){close(fd);unlink(name);return -1;}
    return fd;
}
int eg_stream_peer(int fd,eg_uid_t *uid,eg_gid_t *gid) {
#ifdef EGPU_GUEST_FREESTANDING
    return getpeereid(fd,uid,gid)==0;
#else
    struct ucred peer;eg_socklen_t size=sizeof(peer);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size) || size!=sizeof(peer))return 0;
    *uid=peer.uid;*gid=peer.gid;return 1;
#endif
}
int eg_broker_client_handshake(int fd,const uint8_t token[16],uint8_t identity[64]) {
    uint8_t hello[EG_BROKER_HELLO_BYTES]={0},reply[EG_BROKER_REPLY_BYTES];
    memcpy(hello,"EGB1",4);egpu_put32(hello+4,1);memcpy(hello+8,token,16);
    uint64_t until=eg_stream_now()+5000;
    if(!eg_stream_write(fd,hello,sizeof(hello),until) || !eg_stream_read(fd,reply,sizeof(reply),until))return 0;
    if(memcmp(reply,"EGB1",4) || egpu_u32(reply+4)!=1 || egpu_u32(reply+8) || egpu_u32(reply+12)!=64 ||
       memcmp(reply+16,EGPU_DISK_MAGIC,16) || egpu_u32(reply+32)!=EGPU_VERSION || egpu_u32(reply+36)!=EGPU_DISK_BYTES)return 0;
    memcpy(identity,reply+16,64);return 1;
}
