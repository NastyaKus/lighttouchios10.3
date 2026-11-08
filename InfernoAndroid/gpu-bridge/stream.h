/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "protocol.h"
#ifdef EGPU_GUEST_FREESTANDING
#undef NULL
#include "../guest/darwin-abi.h"
typedef unsigned int eg_uid_t;
typedef unsigned int eg_gid_t;
typedef unsigned int eg_socklen_t;
struct eg_sockaddr_un { uint8_t sun_len,sun_family;char sun_path[104]; };
struct eg_pollfd { int fd;short events,revents; };
struct eg_timespec { long tv_sec,tv_nsec; };
extern int socket(int,int,int),connect(int,const void *,eg_socklen_t),bind(int,const void *,eg_socklen_t);
extern int listen(int,int),accept(int,void *,eg_socklen_t *),setsockopt(int,int,int,const void *,eg_socklen_t);
extern int getsockopt(int,int,int,void *,eg_socklen_t *);
extern int snprintf(char *,size_t,const char *,...);
extern ssize_t send(int,const void *,size_t,int),recv(int,void *,size_t,int);
extern int poll(struct eg_pollfd *,unsigned int,int),clock_gettime(int,struct eg_timespec *);
extern int getpeereid(int,eg_uid_t *,eg_gid_t *),fcntl(int,int,...),pipe(int *),unlink(const char *);
extern int chmod(const char *,unsigned short),chown(const char *,eg_uid_t,eg_gid_t);
extern eg_uid_t getuid(void),geteuid(void);
extern eg_gid_t getgid(void),getegid(void);
extern int setuid(eg_uid_t),setgid(eg_gid_t),setgroups(int,const eg_gid_t *);
extern pid_t getpid(void);
extern int kill(pid_t,int);
extern int rmdir(const char *);
#define EG_AF_UNIX 1
#define EG_SOCK_STREAM 1
#define EG_SOL_SOCKET 0xffff
#define EG_SO_NOSIGPIPE 0x1022
#define EG_SO_ERROR 0x1007
#define EG_F_GETFD 1
#define EG_F_SETFD 2
#define EG_FD_CLOEXEC 1
#define EG_F_GETFL 3
#define EG_F_SETFL 4
#define EG_O_NONBLOCK 4
#define EG_EAGAIN 35
#define EG_EINPROGRESS 36
#define EG_CLOCK_MONOTONIC 6
#define EG_WNOHANG 1
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <grp.h>
#include <signal.h>
typedef uid_t eg_uid_t;
typedef gid_t eg_gid_t;
typedef socklen_t eg_socklen_t;
#define eg_sockaddr_un sockaddr_un
#define eg_pollfd pollfd
#define eg_timespec timespec
#define EG_AF_UNIX AF_UNIX
#define EG_SOCK_STREAM SOCK_STREAM
#define EG_F_GETFD F_GETFD
#define EG_F_SETFD F_SETFD
#define EG_FD_CLOEXEC FD_CLOEXEC
#define EG_F_GETFL F_GETFL
#define EG_F_SETFL F_SETFL
#define EG_O_NONBLOCK O_NONBLOCK
#define EG_SOL_SOCKET SOL_SOCKET
#define EG_SO_ERROR SO_ERROR
#define EG_EAGAIN EAGAIN
#define EG_EINPROGRESS EINPROGRESS
#define EG_CLOCK_MONOTONIC CLOCK_MONOTONIC
#define EG_WNOHANG WNOHANG
#endif
#define EG_POLLIN 1
#define EG_POLLOUT 4
uint64_t eg_stream_now(void);
int eg_stream_wait(int fd,short events,uint64_t deadline);
int eg_stream_read(int fd,void *data,uint32_t bytes,uint64_t deadline);
int eg_stream_write(int fd,const void *data,uint32_t bytes,uint64_t deadline);
int eg_stream_connect(const char *path);
int eg_stream_listen(const char *path);
int eg_stream_peer(int fd,eg_uid_t *uid,eg_gid_t *gid);
int eg_stream_cloexec(int fd,int enabled);
int eg_stream_prepare(int fd);
#define EG_BROKER_HELLO_BYTES 32u
#define EG_BROKER_REPLY_BYTES 80u
int eg_broker_client_handshake(int fd,const uint8_t token[16],uint8_t identity[64]);
