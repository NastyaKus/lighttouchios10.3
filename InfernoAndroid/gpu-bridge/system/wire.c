/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wire.h"
#include "../stream.h"
#ifdef EGPU_GUEST_FREESTANDING
extern void arc4random_buf(void *,size_t);
extern void CCHmac(unsigned int,const void *,size_t,const void *,size_t,void *);
extern unsigned char *CC_SHA256(const void *,unsigned int,unsigned char *);
#else
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif
uint32_t egp2_get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
uint64_t egp2_get64(const uint8_t *p) { return egp2_get32(p)|(uint64_t)egp2_get32(p+4)<<32; }
void egp2_put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8)); }
void egp2_put64(uint8_t *p,uint64_t v) { egp2_put32(p,(uint32_t)v);egp2_put32(p+4,(uint32_t)(v>>32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((unsigned)p[0]|(unsigned)p[1]<<8); }
static void put16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static uint32_t update_crc(uint32_t crc,const uint8_t *p,uint32_t size) {
    for(uint32_t i=0;i<size;i++) {
        crc^=p[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }return crc;
}
static uint32_t frame_crc(const uint8_t *header,const uint8_t *data,uint32_t size) {
    return ~update_crc(update_crc(0xffffffffu,header,24),data,size);
}
static int fields(uint16_t opcode,uint32_t sequence,uint32_t size) {
    unsigned base=opcode&~EGP2_RESPONSE;
    return base>=EGP2_HELLO && base<=EGP2_READBACK_RELEASE && sequence && size<=EGP2_PAYLOAD;
}
int egp2_encode(uint8_t *out,size_t capacity,const Egp2Frame *f) {
    if(!out || !f || !fields(f->opcode,f->sequence,f->size) || (f->size && !f->payload) || capacity<EGP2_HEADER+f->size)return 0;
    memset(out,0,EGP2_HEADER);memcpy(out,"EGP2",4);put16(out+4,2);put16(out+6,f->opcode);
    egp2_put32(out+8,f->sequence);egp2_put32(out+12,f->context);egp2_put32(out+16,f->size);
    if(f->size)memcpy(out+EGP2_HEADER,f->payload,f->size);
    egp2_put32(out+24,frame_crc(out,out+EGP2_HEADER,f->size));return 1;
}
static int header(const uint8_t *p) {
    return !memcmp(p,"EGP2",4) && get16(p+4)==2 && !egp2_get32(p+20) && !egp2_get32(p+28) &&
        fields(get16(p+6),egp2_get32(p+8),egp2_get32(p+16));
}
int egp2_decode(const uint8_t *p,size_t size,Egp2Frame *f) {
    if(!p || !f || size<EGP2_HEADER || !header(p) || size!=EGP2_HEADER+egp2_get32(p+16) ||
       egp2_get32(p+24)!=frame_crc(p,p+EGP2_HEADER,egp2_get32(p+16)))return 0;
    f->opcode=get16(p+6);f->sequence=egp2_get32(p+8);f->context=egp2_get32(p+12);
    f->size=egp2_get32(p+16);f->payload=p+EGP2_HEADER;return 1;
}
int egp2_write(int fd,const Egp2Frame *f,uint64_t deadline) {
    uint8_t *bytes=malloc(EGP2_HEADER+EGP2_PAYLOAD);if(!bytes)return 0;
    int ok=egp2_encode(bytes,EGP2_HEADER+EGP2_PAYLOAD,f) && eg_stream_write(fd,bytes,EGP2_HEADER+f->size,deadline);
    free(bytes);return ok;
}
int egp2_read(int fd,uint8_t buffer[EGP2_HEADER+EGP2_PAYLOAD],Egp2Frame *f,uint64_t deadline) {
    return eg_stream_read(fd,buffer,EGP2_HEADER,deadline) && header(buffer) &&
        eg_stream_read(fd,buffer+EGP2_HEADER,egp2_get32(buffer+16),deadline) &&
        egp2_decode(buffer,EGP2_HEADER+egp2_get32(buffer+16),f);
}
void egp2_sha256(const uint8_t *data,uint32_t size,uint8_t hash[32]) {
#ifdef EGPU_GUEST_FREESTANDING
    CC_SHA256(data,size,hash);
#else
    SHA256(data,size,hash);
#endif
}
void egp2_hmac(const uint8_t secret[32],const uint8_t *data,uint32_t size,uint8_t out[32]) {
#ifdef EGPU_GUEST_FREESTANDING
    CCHmac(2,secret,32,data,size,out);
#else
    unsigned got=32;HMAC(EVP_sha256(),secret,32,data,size,out,&got);
#endif
}
static void proof(const uint8_t secret[32],const uint8_t run[16],const uint8_t client[32],const uint8_t server[32],const char role[6],uint8_t out[32]) {
    uint8_t message[98];memcpy(message,"EGR2AUTH",8);egp2_put32(message+8,2);memcpy(message+12,run,16);
    memcpy(message+28,client,32);memcpy(message+60,server,32);memcpy(message+92,role,6);
    egp2_hmac(secret,message,sizeof(message),out);
}
static int equal(const uint8_t *a,const uint8_t *b,unsigned size) {
    volatile unsigned diff=0;for(unsigned i=0;i<size;i++)diff|=(unsigned)(a[i]^b[i]);return diff==0;
}
int egp2_authenticate(int fd,const uint8_t secret[32],const uint8_t run[16],uint64_t deadline) {
    uint8_t hello[60],reply[76],mac[32];memcpy(hello,"EGR2AUTH",8);egp2_put32(hello+8,2);memcpy(hello+12,run,16);
#ifdef EGPU_GUEST_FREESTANDING
    arc4random_buf(hello+28,32);
#else
    if(RAND_bytes(hello+28,32)!=1)return 0;
#endif
    int ok=eg_stream_write(fd,hello,sizeof(hello),deadline) && eg_stream_read(fd,reply,sizeof(reply),deadline);
    if(ok) {
        proof(secret,run,hello+28,reply+12,"server",mac);
        ok=equal(reply,hello,12) && equal(reply+44,mac,32);
        if(ok){proof(secret,run,hello+28,reply+12,"client",mac);ok=eg_stream_write(fd,mac,32,deadline);}
    }
    memset(hello,0,sizeof(hello));memset(reply,0,sizeof(reply));memset(mac,0,sizeof(mac));return ok;
}
