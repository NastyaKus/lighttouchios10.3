/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define EGP2_HEADER 32u
#define EGP2_PAYLOAD 65536u
#define EGP2_RESPONSE 0x8000u
#define EGP2_MAX_BLOB (64u*1024u*1024u)
enum { EGP2_HELLO=1,EGP2_CONTEXT_CREATE,EGP2_CONTEXT_DESTROY,EGP2_CALL,EGP2_PRESENT,
       EGP2_BLOB_BEGIN,EGP2_BLOB_CHUNK,EGP2_BLOB_COMMIT,EGP2_CLOSE,
       EGP2_READBACK_CALL,EGP2_READBACK_CHUNK,EGP2_READBACK_RELEASE };
#define EGP2_FEATURE_READBACK 1u
typedef struct { uint16_t opcode;uint32_t sequence,context,size;const uint8_t *payload; } Egp2Frame;
uint32_t egp2_get32(const uint8_t *p);
uint64_t egp2_get64(const uint8_t *p);
void egp2_put32(uint8_t *p,uint32_t value);
void egp2_put64(uint8_t *p,uint64_t value);
int egp2_encode(uint8_t *out,size_t capacity,const Egp2Frame *frame);
int egp2_decode(const uint8_t *bytes,size_t size,Egp2Frame *frame);
int egp2_write(int fd,const Egp2Frame *frame,uint64_t deadline);
int egp2_read(int fd,uint8_t buffer[EGP2_HEADER+EGP2_PAYLOAD],Egp2Frame *frame,uint64_t deadline);
void egp2_sha256(const uint8_t *data,uint32_t size,uint8_t hash[32]);
void egp2_hmac(const uint8_t secret[32],const uint8_t *data,uint32_t size,uint8_t hash[32]);
/* Authentication uses distinct random nonces and role-bound HMACs. The secret
 * is passed through an inherited private descriptor, never a command line. */
int egp2_authenticate(int fd,const uint8_t secret[32],const uint8_t run[16],uint64_t deadline);
