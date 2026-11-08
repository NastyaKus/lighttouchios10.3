/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A single diagnostic session. The raw-device capability stays in this root
 * process. Only a random, independent IPC capability crosses exec via fd 3. */
#include "broker.h"
#ifdef EGPU_GUEST_FREESTANDING
static int mount_socket_tmpfs(void) {
    pid_t pid=fork();if(pid<0)return 0;
    if(!pid){char *args[]={"/sbin/mount_tmpfs","-s","1m","/mnt9",NULL};execv(args[0],args);_exit(125);}
    int status=0;uint64_t until=eg_stream_now()+10000;
    for(;;){pid_t result=waitpid(pid,&status,EG_WNOHANG);if(result==pid)return WIFEXITED(status) && !WEXITSTATUS(status);
        if((result<0 && errno!=EINTR) || eg_stream_now()>=until)break;usleep(10000);}
    kill(pid,9);while(waitpid(pid,&status,0)<0 && errno==EINTR){}return 0;
}
#endif
static int random_token(uint8_t token[16]) {
    int fd=open("/dev/urandom",O_RDONLY);if(fd<0)return 0;
    uint32_t at=0;
    while(at<16){ssize_t n=read(fd,token+at,16-at);if(n>0)at+=(uint32_t)n;else if(n==0 || errno!=EINTR)break;}
    close(fd);return at==16;
}
int main(int argc,char **argv) {
    const char *image=NULL,*path=NULL,*child=NULL,*stage="mailbox";char socket_name[104],directory[64]={0};int own_directory=0,own_tmpfs=0;
    eg_uid_t uid;eg_gid_t gid;
#ifdef EGPU_GUEST_FREESTANDING
    (void)argc;(void)argv;
    if(getuid() || geteuid()){dprintf(1,"*** EGPU-BROKER: FAIL launcher requires root\n");return 1;}
    uid=501;gid=501;
    /* Restore root is read-only and /var/run is absent. Only a fresh tmpfs
     * at the existing /mnt9 mountpoint holds the socket; no APFS is mounted. */
    snprintf(directory,sizeof(directory),"/mnt9/ember-gpu-%d",getpid());
    snprintf(socket_name,sizeof(socket_name),"%s/broker.sock",directory);path=socket_name;
    child="/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-11.04-012.2-P.bin";
#else
    (void)socket_name;
    if(argc!=7 || strcmp(argv[1],"--image") || strcmp(argv[3],"--socket") || strcmp(argv[5],"--client"))return 2;
    image=argv[2];path=argv[4];child=argv[6];uid=geteuid();gid=getegid();
#endif
    EgMailbox mailbox;EgBrokerResult result={0};uint8_t token[16];int listener=-1,channel[2]={-1,-1},status=0,ok=0;pid_t pid=-1;
    if(!eg_mailbox_open(&mailbox,image)){dprintf(1,"*** EGPU-BROKER: FAIL marked mailbox unavailable\n");return 1;}
    stage="random";if(!random_token(token))goto cleanup;
#ifdef EGPU_GUEST_FREESTANDING
    stage="socket tmpfs";if(!mount_socket_tmpfs())goto cleanup;own_tmpfs=1;
    stage="directory";if(mkdir(directory,0750))goto cleanup;own_directory=1;
    if(chown(directory,0,gid))goto cleanup;
#endif
    stage="listen";if((listener=eg_stream_listen(path))<0)goto cleanup;
    stage="socket permissions";
#ifdef EGPU_GUEST_FREESTANDING
    if(chown(path,0,gid) || chmod(path,0660))goto cleanup;
#else
    if(chmod(path,0600))goto cleanup;
#endif
    stage="capability pipe";if(pipe(channel) || !eg_stream_cloexec(channel[0],1) || !eg_stream_cloexec(channel[1],1))goto cleanup;
    if(write(channel[1],token,sizeof(token))!=sizeof(token))goto cleanup;
    close(channel[1]);channel[1]=-1;
    stage="fork";pid=fork();if(pid<0)goto cleanup;
    if(!pid) {
        close(listener);close(mailbox.fd);
        if(channel[0]!=3){if(dup2(channel[0],3)<0)_exit(121);close(channel[0]);}
        if(!eg_stream_cloexec(3,0))_exit(122);
        memset(token,0,sizeof(token));
#ifdef EGPU_GUEST_FREESTANDING
        if(setgroups(0,NULL) || setgid(gid) || setuid(uid))_exit(123);
#endif
        char *args[]={(char *)child,"--broker",(char *)path,"--forbidden",mailbox.device,NULL};
        execv(child,args);dprintf(1,"*** EGPU-BROKER-CLIENT: FAIL exec errno=%d\n",errno);_exit(124);
    }
    close(channel[0]);channel[0]=-1;
    stage="serve";ok=eg_broker_serve(listener,&mailbox,token,uid,gid,&result);
    memset(token,0,sizeof(token));
    /* A successful GPU DESTROY is insufficient: the client must exit cleanly. */
    uint64_t until=eg_stream_now()+5000;pid_t waited=0;
    do{waited=waitpid(pid,&status,EG_WNOHANG);if(waited==pid || (waited<0 && errno!=EINTR))break;usleep(10000);}while(eg_stream_now()<until);
    if(waited!=pid){kill(pid,9);while(waitpid(pid,&status,0)<0 && errno==EINTR){}ok=0;}
    pid=-1;ok=ok && WIFEXITED(status) && !WEXITSTATUS(status);
cleanup:
    if(pid>0){kill(pid,9);while(waitpid(pid,&status,0)<0 && errno==EINTR){}}
    if(channel[0]>=0)close(channel[0]);
    if(channel[1]>=0)close(channel[1]);
    int saved_errno=errno;
    if(listener>=0){close(listener);unlink(path);}
    if(own_directory)rmdir(directory);
#ifdef EGPU_GUEST_FREESTANDING
    if(own_tmpfs && unmount("/mnt9",0)){ok=0;stage="tmpfs cleanup";saved_errno=errno;}
#else
    (void)own_tmpfs;
#endif
    eg_mailbox_close(&mailbox);memset(token,0,sizeof(token));
    if(ok)dprintf(1,"*** EGPU-BROKER: PASS uid=%u gid=%u forwarded=%u replayed=%u child_exit=0 cleanup=1\n",(unsigned)uid,(unsigned)gid,result.forwarded,result.replayed);
    else dprintf(1,"*** EGPU-BROKER: FAIL stage=%s errno=%d child_status=%d\n",stage,saved_errno,status);
    return ok?0:1;
}
