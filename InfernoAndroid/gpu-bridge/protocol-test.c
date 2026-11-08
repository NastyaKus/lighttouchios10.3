/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t page[EGPU_PAGE],payload[]={9,8,7,6};EgpuFrame frame;
    assert(egpu_crc32("123456789",9,0)==0xcbf43926u);
    assert(egpu_crc32("56789",5,egpu_crc32("1234",4,0))==0xcbf43926u);
    assert(!egpu_encode(page,EGPU_CLEAR,0x01020304,payload,4));
    /* Independent Python struct.pack + zlib.crc32 golden packet. */
    const uint8_t golden[]={0x45,0x47,0x50,0x31,1,0,3,0,4,3,2,1,4,0,0,0,0x2b,0xce,0xfb,0x36,9,8,7,6};
    assert(!memcmp(page,golden,sizeof(golden)));
    assert(!egpu_decode(page,24,&frame) && frame.seq==0x01020304 && frame.opcode==EGPU_CLEAR && frame.size==4);
    for(size_t n=0;n<24;n++)assert(egpu_decode(page,n,&frame)==EGPU_BAD_FRAME);
    for(size_t i=0;i<24;i++){page[i]^=1;assert(egpu_decode(page,EGPU_PAGE,&frame));page[i]^=1;}
    assert(egpu_encode(page,1,0,payload,4));
    assert(egpu_encode(page,1,1,payload,EGPU_PAYLOAD+1));
    assert(egpu_encode(page,1,1,NULL,4));
    assert(!egpu_encode(page,1,1,NULL,0));assert(!egpu_decode(page,20,&frame));
    assert(!egpu_decode(page,EGPU_PAGE,&frame));page[EGPU_PAGE-1]=1;
    assert(!egpu_decode(page,EGPU_PAGE,&frame)); /* Transport padding is not payload. */
    uint32_t random=1;
    for(int trial=0;trial<20000;trial++) {
        for(unsigned i=0;i<EGPU_PAGE;i++){random=random*1664525u+1013904223u;page[i]=random>>24;}
        assert(egpu_decode(page,(random%EGPU_PAGE),&frame)==EGPU_BAD_FRAME);
    }
    puts("PASS protocol: independent CRC/LE vector, short/truncated/corrupt packets, bounds, 20000 malformed frames");return 0;
}
