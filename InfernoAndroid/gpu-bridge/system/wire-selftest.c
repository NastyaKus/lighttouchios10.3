/* SPDX-License-Identifier: GPL-3.0-or-later
 * Known SHA256 / RFC4231 HMAC vectors and hostile framing, same code on iOS. */
#include "wire.h"
#include "../stream.h"
int egp2_selftest(void) {
    const uint8_t sha[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    const uint8_t hmac[32]={0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7};
    uint8_t got[32],key[32]={0};memset(key,0x0b,20);
    egp2_sha256((const uint8_t *)"abc",3,got);if(memcmp(got,sha,32))return 1;
    egp2_hmac(key,(const uint8_t *)"Hi There",8,got);if(memcmp(got,hmac,32))return 2;
    uint8_t payload[]={1,2,3},bytes[35];Egp2Frame input={EGP2_CALL,0x12345678,0x90abcdef,3,payload},parsed;
    if(!egp2_encode(bytes,sizeof(bytes),&input) || !egp2_decode(bytes,sizeof(bytes),&parsed) ||
       parsed.context!=input.context || parsed.sequence!=input.sequence || parsed.size!=3 || memcmp(parsed.payload,payload,3))return 3;
    for(unsigned i=0;i<sizeof(bytes);i++) {
        bytes[i]^=1;if(egp2_decode(bytes,sizeof(bytes),&parsed))return 4;bytes[i]^=1;
        if(egp2_decode(bytes,i,&parsed))return 5;
    }
    if(egp2_encode(bytes,sizeof(bytes)-1,&input))return 6;
    input.sequence=0;if(egp2_encode(bytes,sizeof(bytes),&input))return 7;
    return 0;
}
#ifdef EGP2_SELFTEST_MAIN
int main(void) { int result=egp2_selftest();dprintf(1,"EGP2 wire/crypto selftest %s code=%d\n",result?"FAIL":"PASS",result);return result; }
#endif
