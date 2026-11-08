/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Experimental command ABI, not an Apple/Metal/IOAccelerator ABI. All integers
 * and IEEE754 floats are little endian. CRC covers header[0..15] + payload.
 * One bounded request at a time; no host pointers or GLuint names cross it. */
#define EGPU_PAGE 4096u
#define EGPU_HEADER 20u
#define EGPU_PAYLOAD (EGPU_PAGE-EGPU_HEADER)
#define EGPU_VERSION 1u
#define EGPU_RESPONSE 0x8000u
#define EGPU_DISK_MAGIC "EMBER_GPU_PROBE1"
#define EGPU_DISK_BYTES (16u*1024u*1024u)
#define EGPU_REQUEST_OFFSET 4096u
#define EGPU_REPLY_OFFSET 8192u
enum { EGPU_HELLO=1, EGPU_CREATE=2, EGPU_CLEAR=3, EGPU_TRIANGLE=4,
       EGPU_READ_POINTS=5, EGPU_FENCE=6, EGPU_DESTROY=7 };
enum { EGPU_OK=0, EGPU_BAD_FRAME=1, EGPU_BAD_COMMAND=2, EGPU_BAD_ARGUMENT=3,
       EGPU_BAD_HANDLE=4, EGPU_GL_ERROR=5, EGPU_TIMEOUT=6, EGPU_SEQUENCE=7 };
typedef struct { uint16_t opcode; uint32_t seq, size; const uint8_t *payload; } EgpuFrame;
uint16_t egpu_u16(const void *p);
uint32_t egpu_u32(const void *p);
void egpu_put16(void *p, uint16_t value);
void egpu_put32(void *p, uint32_t value);
uint32_t egpu_crc32(const void *data, size_t size, uint32_t prior);
int egpu_decode(const uint8_t *data, size_t size, EgpuFrame *out);
int egpu_encode(uint8_t page[EGPU_PAGE], uint16_t opcode, uint32_t seq,
                const void *payload, uint32_t size);
