/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "wire.h"
typedef struct {
    int fd;
    uint32_t sequence, features, maximum;
    uint64_t next_blob;
    uint8_t *buffer;
    char renderer[1025];
} Egp2Client;
/* Takes ownership of a connected nonblocking fd. For a root broker use auth;
 * attach is for an already authenticated broker channel, never a raw socket.
 * Neither function retains or logs the per-boot secret. */
int egp2_client_auth(Egp2Client *, int fd, const uint8_t secret[32], const uint8_t run[16]);
int egp2_client_attach(Egp2Client *, int fd);
int egp2_client_create(Egp2Client *, uint32_t id, uint32_t share, uint32_t width, uint32_t height);
int egp2_client_destroy(Egp2Client *, uint32_t id);
/* Result is caller-owned (free). Large explicit readbacks are SHA-256 checked
 * after contiguous chunk delivery. NULL/zero-size is a successful void reply. */
int egp2_client_call(Egp2Client *, uint32_t context, const uint8_t *, uint32_t, const uint8_t *upload,
                     uint32_t upload_size, int readback, uint8_t **result, uint32_t *result_size);
int egp2_client_present(Egp2Client *, uint32_t context, uint64_t surface, uint64_t serial, uint32_t *status);
void egp2_client_close(Egp2Client *);
