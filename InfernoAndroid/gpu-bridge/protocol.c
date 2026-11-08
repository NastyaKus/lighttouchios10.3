/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "protocol.h"
#ifdef EGPU_GUEST_FREESTANDING
#undef NULL
#include "../guest/darwin-abi.h"
#else
#include <string.h>
#endif
uint16_t egpu_u16(const void *p) { const uint8_t *b=p; return b[0]|(uint16_t)b[1]<<8; }
uint32_t egpu_u32(const void *p) { const uint8_t *b=p; return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24; }
void egpu_put16(void *p,uint16_t v) { uint8_t *b=p; b[0]=v; b[1]=v>>8; }
void egpu_put32(void *p,uint32_t v) { uint8_t *b=p; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; }
uint32_t egpu_crc32(const void *data,size_t size,uint32_t prior) {
    const uint8_t *p=data; uint32_t crc=~prior;
    for(size_t i=0;i<size;i++) { crc^=p[i]; for(int j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u))); }
    return ~crc;
}
int egpu_decode(const uint8_t *data,size_t size,EgpuFrame *out) {
    if(!data || !out || size<EGPU_HEADER || memcmp(data,"EGP1",4) || egpu_u16(data+4)!=EGPU_VERSION) return EGPU_BAD_FRAME;
    uint32_t n=egpu_u32(data+12);
    if(n>EGPU_PAYLOAD || n>size-EGPU_HEADER || egpu_u32(data+8)==0) return EGPU_BAD_FRAME;
    uint32_t crc=egpu_crc32(data,16,0);crc=egpu_crc32(data+EGPU_HEADER,n,crc);
    if(crc!=egpu_u32(data+16)) return EGPU_BAD_FRAME;
    *out=(EgpuFrame){egpu_u16(data+6),egpu_u32(data+8),n,data+EGPU_HEADER}; return EGPU_OK;
}
int egpu_encode(uint8_t page[EGPU_PAGE],uint16_t op,uint32_t seq,const void *payload,uint32_t size) {
    if(!page || !seq || size>EGPU_PAYLOAD || (size && !payload)) return EGPU_BAD_FRAME;
    memset(page,0,EGPU_PAGE);memcpy(page,"EGP1",4);egpu_put16(page+4,EGPU_VERSION);egpu_put16(page+6,op);
    egpu_put32(page+8,seq);egpu_put32(page+12,size);
    if(size)memcpy(page+EGPU_HEADER,payload,size);
    uint32_t crc=egpu_crc32(page,16,0);egpu_put32(page+16,egpu_crc32(page+EGPU_HEADER,size,crc));return EGPU_OK;
}
