/* SPDX-License-Identifier: GPL-3.0-or-later
 * Cross-language transport fixture. NOT an iOS GPU acceleration test. */
#include "wire.h"
#include "../stream.h"
#ifdef EGPU_GUEST_FREESTANDING
struct address { uint8_t length,family;uint16_t port;uint32_t ipv4;uint8_t pad[8]; };
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
static uint8_t buffer[EGP2_HEADER+EGP2_PAYLOAD];
static uint32_t sequence;
static int rpc(int fd,unsigned opcode,unsigned context,const uint8_t *p,unsigned size,Egp2Frame *reply) {
    Egp2Frame request={(uint16_t)opcode,++sequence,context,size,p};uint64_t until=eg_stream_now()+5000;
    return egp2_write(fd,&request,until) && egp2_read(fd,buffer,reply,until) &&
        reply->opcode==(opcode|EGP2_RESPONSE) && reply->sequence==sequence && reply->context==context &&
        reply->size>=4 && !egp2_get32(reply->payload);
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    unsigned port=0;for(const char *p=argv[1];*p;p++){if(*p<'0' || *p>'9' || port>6553)return 2;port=port*10+(unsigned)(*p-'0');}
    if(!port || port>65535)return 2;
    uint8_t capability[48];unsigned at=0;
    while(at<sizeof(capability)){ssize_t n=read(0,capability+at,sizeof(capability)-at);if(n<=0)return 3;at+=(unsigned)n;}
    int fd=socket(2,1,0);if(fd<0)return 4;
#ifdef EGPU_GUEST_FREESTANDING
    struct address address={sizeof(address),2,(uint16_t)((port>>8)|(port<<8)),0x0202000a,{0}}; /* slirp 10.0.2.2 */
#else
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((uint16_t)port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
#endif
    if(connect(fd,(const void *)&address,sizeof(address)) || !eg_stream_prepare(fd))return 4;
    int ok=egp2_authenticate(fd,capability,capability+32,eg_stream_now()+5000);memset(capability,0,sizeof(capability));if(!ok)return 5;
    uint8_t p[EGP2_PAYLOAD]={0};Egp2Frame reply;egp2_put32(p,3);
    if(!rpc(fd,EGP2_HELLO,0,p,16,&reply) || reply.size<20 || egp2_get32(reply.payload+4)<3)return 6;
    egp2_put32(p,828);egp2_put32(p+4,1792);egp2_put32(p+8,0);
    if(!rpc(fd,EGP2_CONTEXT_CREATE,1,p,12,&reply))return 7;
    const unsigned bytes=2*1024*1024+91;uint8_t *data=malloc(bytes),hash[32];if(!data)return 8;
    for(unsigned i=0;i<bytes;i++)data[i]=(uint8_t)(i*13);
    egp2_sha256(data,bytes,hash);egp2_put64(p,7);egp2_put32(p+8,bytes);memcpy(p+12,hash,32);
    if(!rpc(fd,EGP2_BLOB_BEGIN,1,p,44,&reply))return 9;
    for(at=0;at<bytes;) {
        unsigned n=bytes-at;if(n>EGP2_PAYLOAD-12)n=EGP2_PAYLOAD-12;
        egp2_put64(p,7);egp2_put32(p+8,at);memcpy(p+12,data+at,n);
        if(!rpc(fd,EGP2_BLOB_CHUNK,1,p,12+n,&reply))return 10;
        at+=n;
    }
    free(data);egp2_put64(p,7);
    if(!rpc(fd,EGP2_BLOB_COMMIT,1,p,8,&reply))return 11;
    egp2_put32(p+8,42);
    if(!rpc(fd,EGP2_CALL,1,p,12,&reply) || reply.size!=36 || memcmp(reply.payload+4,hash,32))return 12;
    if(!rpc(fd,EGP2_CLOSE,0,NULL,0,&reply))return 13;
    close(fd);dprintf(1,"EGP2 C/Kotlin PASS frames=%u bytes=%u GPU_execution=not_tested\n",sequence,bytes);return 0;
}
