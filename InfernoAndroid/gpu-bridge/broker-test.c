/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "broker.h"
#include <assert.h>
/* Mailbox fixture: broker authorization/framing is tested independently of GL
 * and raw devices. End-to-end tests use the real mailbox implementation. */
int eg_mailbox_exchange(EgMailbox *m,const uint8_t request[EGPU_PAGE],uint8_t reply[EGPU_PAGE]) {
    (void)m;EgpuFrame f;assert(!egpu_decode(request,EGPU_PAGE,&f));
    uint8_t status[4]={0};assert(!egpu_encode(reply,f.opcode|EGPU_RESPONSE,f.seq,status,4));return 1;
}
static const uint8_t token[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static char directory[128],path[160];
static int reports;static pid_t server;
static void start(int wrong_uid) {
    int listener=eg_stream_listen(path);assert(listener>=0);
    int channel[2];assert(!pipe(channel));server=fork();assert(server>=0);
    if(!server) {
        close(channel[0]);EgMailbox m={0};memcpy(m.identity,EGPU_DISK_MAGIC,16);
        egpu_put32(m.identity+16,EGPU_VERSION);egpu_put32(m.identity+20,EGPU_DISK_BYTES);
        EgBrokerResult result;int ok=eg_broker_serve(listener,&m,token,geteuid()+wrong_uid,getegid(),&result);
        assert(write(channel[1],&result,sizeof(result))==sizeof(result));close(channel[1]);close(listener);_exit(ok?0:1);
    }
    close(listener);close(channel[1]);reports=channel[0];
}
static EgBrokerResult finish(int expected_exit) {
    EgBrokerResult result;assert(eg_stream_read(reports,&result,sizeof(result),eg_stream_now()+5000));close(reports);
    int status;assert(waitpid(server,&status,0)==server && WIFEXITED(status) && WEXITSTATUS(status)==expected_exit);
    assert(!unlink(path));return result;
}
static int connect_client(void) {
    int fd=eg_stream_connect(path);assert(fd>=0);eg_uid_t uid;eg_gid_t gid;
    assert(eg_stream_peer(fd,&uid,&gid) && uid==geteuid() && gid==getegid());
    assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);assert(fcntl(fd,F_GETFL)&O_NONBLOCK);return fd;
}
static int authorize(void) {
    int fd=connect_client();uint8_t identity[64];assert(eg_broker_client_handshake(fd,token,identity));
    assert(!memcmp(identity,EGPU_DISK_MAGIC,16));return fd;
}
static void packet(uint8_t page[EGPU_PAGE],uint16_t op,uint32_t seq) { assert(!egpu_encode(page,op,seq,NULL,0)); }
static void transact(int fd,uint8_t page[EGPU_PAGE],int fragmented) {
    uint8_t reply[EGPU_PAGE];uint64_t until=eg_stream_now()+2000;
    if(fragmented)for(unsigned i=0;i<EGPU_PAGE;i+=7)assert(eg_stream_write(fd,page+i,EGPU_PAGE-i<7?EGPU_PAGE-i:7,until));
    else assert(eg_stream_write(fd,page,EGPU_PAGE,until));
    assert(eg_stream_read(fd,reply,sizeof(reply),until));EgpuFrame f;
    assert(!egpu_decode(reply,sizeof(reply),&f) && f.seq==egpu_u32(page+8) && !egpu_u32(f.payload));
}
static void happy_replay_and_fragmentation(void) {
    start(0);int fd=connect_client();uint8_t hello[32]={0},reply[80],page[EGPU_PAGE];
    memcpy(hello,"EGB1",4);egpu_put32(hello+4,1);memcpy(hello+8,token,16);
    for(unsigned i=0;i<sizeof(hello);i++)assert(eg_stream_write(fd,hello+i,1,eg_stream_now()+2000));
    assert(eg_stream_read(fd,reply,sizeof(reply),eg_stream_now()+2000) && !egpu_u32(reply+8));
    packet(page,EGPU_HELLO,1);transact(fd,page,1);transact(fd,page,0);
    packet(page,EGPU_DESTROY,2);transact(fd,page,0);close(fd);
    EgBrokerResult r=finish(0);assert(r.complete && r.forwarded==2 && r.replayed==1 && !r.rejected);
}
static void rejected_authentication(void) {
    start(0);
    for(int i=0;i<6;i++) {
        int fd=connect_client();uint8_t hello[32]={0},reply[80];memcpy(hello,"EGB1",4);egpu_put32(hello+4,1);memcpy(hello+8,token,16);
        if(i==0)hello[8]^=1;else if(i==1)hello[31]=1;else if(i==2)hello[0]='X';else if(i==3)hello[4]=2;
        if(i<4){assert(eg_stream_write(fd,hello,32,eg_stream_now()+1000));assert(eg_stream_read(fd,reply,80,eg_stream_now()+1000));assert(egpu_u32(reply+8)==1);for(int j=12;j<80;j++)assert(!reply[j]);}
        else if(i==4)assert(eg_stream_write(fd,hello,15,eg_stream_now()+1000));
        else if(i==5)assert(eg_stream_write(fd,hello,0,eg_stream_now()+1000));
        close(fd);
    }
    int fd=authorize();uint8_t page[EGPU_PAGE];packet(page,EGPU_HELLO,1);transact(fd,page,0);packet(page,EGPU_DESTROY,2);transact(fd,page,0);close(fd);
    EgBrokerResult r=finish(0);assert(r.rejected==6 && r.forwarded==2);
    start(1);
    for(int i=0;i<8;i++){fd=connect_client();uint8_t id[64];assert(!eg_broker_client_handshake(fd,token,id));close(fd);}
    r=finish(1);assert(r.rejected==8 && !r.forwarded && !r.complete);
}
static void malformed_frames(void) {
    for(int mode=0;mode<7;mode++) {
        start(0);int fd=authorize();uint8_t page[EGPU_PAGE];int first=(mode==2 || mode==3);
        if(first){packet(page,EGPU_HELLO,1);transact(fd,page,0);}
        packet(page,EGPU_HELLO,1);
        if(mode==0)page[16]^=1; /* Bad CRC. */
        if(mode==1)packet(page,EGPU_FENCE,1); /* First command must be HELLO. */
        if(mode==2)packet(page,EGPU_FENCE,1); /* Changed duplicate. */
        if(mode==3)packet(page,EGPU_FENCE,3); /* Sequence gap. */
        if(mode==4)packet(page,EGPU_HELLO|EGPU_RESPONSE,1);
        if(mode==5){egpu_put32(page+8,0);egpu_put32(page+16,egpu_crc32(page,16,0));}
        uint32_t bytes=mode==6?100:EGPU_PAGE;
        assert(eg_stream_write(fd,page,bytes,eg_stream_now()+1000));
        if(mode==6)shutdown(fd,SHUT_WR);
        assert(!eg_stream_read(fd,page,EGPU_PAGE,eg_stream_now()+2000));close(fd);
        EgBrokerResult r=finish(1);assert(!r.complete && !r.replayed && r.forwarded==(unsigned)first);
    }
}
static void deadlines_and_endpoint_ownership(void) {
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);assert(write(fd,"keep",4)==4);
    assert(eg_stream_listen(path)<0);char text[4];assert(pread(fd,text,4,0)==4 && !memcmp(text,"keep",4));close(fd);unlink(path);
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));assert(eg_stream_prepare(pair[0]));
    uint8_t byte;uint64_t start=eg_stream_now();assert(!eg_stream_read(pair[0],&byte,1,start+30));assert(eg_stream_now()-start<500);
    close(pair[1]);assert(!eg_stream_write(pair[0],"x",1,eg_stream_now()+30));close(pair[0]); /* No SIGPIPE. */
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));assert(eg_stream_prepare(pair[0]));
    int bytes=1024;assert(!setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&bytes,sizeof(bytes)));
    uint8_t *bulk=calloc(1,1<<20);assert(bulk);start=eg_stream_now();assert(!eg_stream_write(pair[0],bulk,1<<20,start+30));assert(eg_stream_now()-start<500);
    free(bulk);close(pair[0]);close(pair[1]);
}
int main(void) {
    snprintf(directory,sizeof(directory),"/tmp/ember-broker-test-XXXXXX");assert(mkdtemp(directory));snprintf(path,sizeof(path),"%s/sock",directory);
    deadlines_and_endpoint_ownership();happy_replay_and_fragmentation();rejected_authentication();malformed_frames();
    assert(!rmdir(directory));puts("broker: credentials, capability, framing, replay, fragmentation, deadlines and endpoint ownership PASS");return 0;
}
