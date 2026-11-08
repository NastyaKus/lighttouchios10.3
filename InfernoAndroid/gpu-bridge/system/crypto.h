/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    unsigned used;
} Egp2Sha256;
void egp2_sha256_init(Egp2Sha256 *);
void egp2_sha256_update(Egp2Sha256 *, const void *, size_t);
void egp2_sha256_final(Egp2Sha256 *, uint8_t out[32]);
void egp2_builtin_sha256(const uint8_t *, uint32_t, uint8_t out[32]);
void egp2_builtin_hmac(const uint8_t key[32], const uint8_t *, uint32_t, uint8_t out[32]);
