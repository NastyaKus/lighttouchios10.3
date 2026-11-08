/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "client.h"
#include "commands.h"
#include "../stream.h"
static int stop(Egp2Client *c) {
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    free(c->buffer);
    c->buffer = NULL;
    return 0;
}
static int rpc(Egp2Client *c, uint16_t opcode, uint32_t context, const uint8_t *p, uint32_t size, Egp2Frame *r) {
    if (c->fd < 0 || !c->buffer || c->sequence == UINT32_MAX)
        return stop(c);
    Egp2Frame request = {opcode, ++c->sequence, context, size, p};
    uint64_t until = eg_stream_now() + 15000;
    if (!egp2_write(c->fd, &request, until) || !egp2_read(c->fd, c->buffer, r, until) ||
        r->opcode != (opcode | EGP2_RESPONSE) || r->sequence != c->sequence || r->context != context || r->size < 4 ||
        egp2_get32(r->payload))
        return stop(c);
    r->payload += 4;
    r->size -= 4;
    return 1;
}
int egp2_client_attach(Egp2Client *c, int fd) {
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->next_blob = 1;
    c->features = EGP2_FEATURE_READBACK;
    c->buffer = malloc(EGP2_HEADER + EGP2_PAYLOAD);
    if (fd < 0 || !c->buffer || !eg_stream_prepare(fd))
        return stop(c);
    uint8_t hello[16] = {0};
    egp2_put32(hello, 3);
    egp2_put32(hello + 8, c->features);
    Egp2Frame r;
    if (!rpc(c, EGP2_HELLO, 0, hello, sizeof(hello), &r) || r.size < 16 || egp2_get32(r.payload) < 3 ||
        egp2_get32(r.payload + 8) < 2048 || egp2_get32(r.payload + 8) > 65536)
        return stop(c);
    uint32_t len = egp2_get32(r.payload + 12);
    if (!len || len > 1024 || r.size != 16 + len || memchr(r.payload + 16, 0, len))
        return stop(c);
    memcpy(c->renderer, r.payload + 16, len);
    c->renderer[len] = 0;
    c->maximum = egp2_get32(r.payload + 8);
    return 1;
}
int egp2_client_auth(Egp2Client *c, int fd, const uint8_t secret[32], const uint8_t run[16]) {
    if (fd < 0 || !eg_stream_prepare(fd) || !egp2_authenticate(fd, secret, run, eg_stream_now() + 15000)) {
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        return stop(c);
    }
    return egp2_client_attach(c, fd);
}
int egp2_client_create(Egp2Client *c, uint32_t id, uint32_t share, uint32_t w, uint32_t h) {
    uint8_t p[12];
    egp2_put32(p, w);
    egp2_put32(p + 4, h);
    egp2_put32(p + 8, share);
    Egp2Frame r;
    return rpc(c, EGP2_CONTEXT_CREATE, id, p, sizeof(p), &r) && (!r.size || stop(c));
}
int egp2_client_destroy(Egp2Client *c, uint32_t id) {
    Egp2Frame r;
    return rpc(c, EGP2_CONTEXT_DESTROY, id, NULL, 0, &r) && (!r.size || stop(c));
}
static int upload_blob(Egp2Client *c, uint32_t context, const uint8_t *data, uint32_t size, uint64_t *id) {
    if (!size) {
        *id = 0;
        return 1;
    }
    if (!data || size > EGP2_MAX_BLOB || c->next_blob > INT64_MAX)
        return stop(c);
    *id = c->next_blob++;
    uint8_t p[EGP2_PAYLOAD], hash[32];
    egp2_sha256(data, size, hash);
    egp2_put64(p, *id);
    egp2_put32(p + 8, size);
    memcpy(p + 12, hash, 32);
    Egp2Frame r;
    if (!rpc(c, EGP2_BLOB_BEGIN, context, p, 44, &r) || r.size)
        return stop(c);
    for (uint32_t at = 0; at < size;) {
        uint32_t n = size - at;
        if (n > EGP2_PAYLOAD - 12)
            n = EGP2_PAYLOAD - 12;
        egp2_put64(p, *id);
        egp2_put32(p + 8, at);
        memcpy(p + 12, data + at, n);
        if (!rpc(c, EGP2_BLOB_CHUNK, context, p, n + 12, &r) || r.size)
            return stop(c);
        at += n;
    }
    egp2_put64(p, *id);
    return rpc(c, EGP2_BLOB_COMMIT, context, p, 8, &r) && (!r.size || stop(c));
}
int egp2_client_call(Egp2Client *c, uint32_t context, const uint8_t *command, uint32_t size, const uint8_t *upload,
                     uint32_t bytes, int readback, uint8_t **result, uint32_t *result_size) {
    *result = NULL;
    *result_size = 0;
    if (!command || size < 4 || size > EGS_MAX_COMMAND_BYTES)
        return stop(c);
    uint64_t id;
    if (!upload_blob(c, context, upload, bytes, &id))
        return 0;
    uint8_t *p = malloc(size + 8);
    if (!p)
        return stop(c);
    egp2_put64(p, id);
    memcpy(p + 8, command, size);
    Egp2Frame r;
    int ok = rpc(c, readback ? EGP2_READBACK_CALL : EGP2_CALL, context, p, size + 8, &r);
    free(p);
    if (!ok)
        return 0;
    if (!readback) {
        if (r.size) {
            *result = malloc(r.size);
            if (!*result)
                return stop(c);
            memcpy(*result, r.payload, r.size);
        }
        *result_size = r.size;
        return 1;
    }
    if (r.size != 44)
        return stop(c);
    uint64_t download = egp2_get64(r.payload);
    uint32_t total = egp2_get32(r.payload + 8);
    uint8_t expected[32];
    memcpy(expected, r.payload + 12, 32);
    if (!download || download > INT64_MAX || total > EGP2_MAX_BLOB)
        return stop(c);
    uint8_t *data = total ? malloc(total) : NULL;
    if (total && !data)
        return stop(c);
    uint8_t request[16];
    egp2_put64(request, download);
    for (uint32_t at = 0; at < total;) {
        uint32_t n = total - at;
        if (n > EGP2_PAYLOAD - 16)
            n = EGP2_PAYLOAD - 16;
        egp2_put32(request + 8, at);
        egp2_put32(request + 12, n);
        if (!rpc(c, EGP2_READBACK_CHUNK, context, request, 16, &r) || r.size != 12 + n ||
            egp2_get64(r.payload) != download || egp2_get32(r.payload + 8) != at) {
            free(data);
            return stop(c);
        }
        memcpy(data + at, r.payload + 12, n);
        at += n;
    }
    uint8_t actual[32];
    egp2_sha256(data, total, actual);
    if (memcmp(actual, expected, 32) || !rpc(c, EGP2_READBACK_RELEASE, context, request, 8, &r) || r.size) {
        free(data);
        return stop(c);
    }
    *result = data;
    *result_size = total;
    return 1;
}
int egp2_client_present(Egp2Client *c, uint32_t context, uint64_t surface, uint64_t serial, uint32_t *status) {
    uint8_t p[16];
    egp2_put64(p, surface);
    egp2_put64(p + 8, serial);
    Egp2Frame r;
    if (!rpc(c, EGP2_PRESENT, context, p, 16, &r) || r.size != 4)
        return stop(c);
    *status = egp2_get32(r.payload);
    return *status <= 1 || stop(c);
}
void egp2_client_close(Egp2Client *c) {
    if (c->fd >= 0 && c->buffer) {
        Egp2Frame r;
        rpc(c, EGP2_CLOSE, 0, NULL, 0, &r);
    }
    stop(c);
}
