/* SPDX-License-Identifier: GPL-3.0-or-later
 * Run by `adb shell` (uid shell) to copy one file from the working Ember VM's
 * Android/data folder into the side-by-side test build's folder.
 *
 *   ember-import copy SOURCE DESTINATION
 *   ember-import hash FILE
 *
 * copy never writes SOURCE (it is opened O_RDONLY). Zero 64 KiB blocks are
 * skipped with lseek, so a sparse 34 GB raw root stays as small as its data.
 * The result is written to DESTINATION.part, fsynced, read back, and renamed
 * over DESTINATION only when the SHA-256 of what was read from SOURCE equals
 * the SHA-256 read back from the new file. Output, one line:
 *   OK <logical bytes> <written bytes> <sha256>
 *   FAIL <stage> <errno> <message>
 * Exit status 0 only for OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    unsigned used;
} Sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

#if defined(__aarch64__)
#include <arm_neon.h>
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
/* ARMv8 SHA-256 instructions: almost every arm64 phone has them, and hashing
 * a 34 GB disk twice is otherwise the slowest part of the import. */
__attribute__((target("+crypto"))) static void sha_block_arm(Sha256 *s, const uint8_t *p) {
    uint32x4_t abcd = vld1q_u32(s->h), efgh = vld1q_u32(s->h + 4), abcd0 = abcd, efgh0 = efgh;
    uint32x4_t m[4];
    for (int i = 0; i < 4; i++)
        m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
    for (int i = 0; i < 16; i++) {
        uint32x4_t w = vaddq_u32(m[i & 3], vld1q_u32(K + 4 * i)), t = abcd;
        if (i < 12)
            m[i & 3] = vsha256su1q_u32(vsha256su0q_u32(m[i & 3], m[(i + 1) & 3]), m[(i + 2) & 3], m[(i + 3) & 3]);
        abcd = vsha256hq_u32(abcd, efgh, w);
        efgh = vsha256h2q_u32(efgh, t, w);
    }
    vst1q_u32(s->h, vaddq_u32(abcd, abcd0));
    vst1q_u32(s->h + 4, vaddq_u32(efgh, efgh0));
}
static int arm_sha2 = -1;
static void sha_block_any(Sha256 *s, const uint8_t *p) {
    if (arm_sha2 < 0)
        arm_sha2 = (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
    if (arm_sha2)
        sha_block_arm(s, p);
    else
        sha_block(s, p);
}
#define SHA_BLOCK sha_block_any
#else
#define SHA_BLOCK sha_block
#endif

static void sha_init(Sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv));
    s->length = 0;
    s->used = 0;
}

static void sha_update(Sha256 *s, const uint8_t *p, size_t n) {
    s->length += n;
    if (s->used) {
        while (n && s->used < 64)
            s->block[s->used++] = *p++, n--;
        if (s->used < 64)
            return;
        SHA_BLOCK(s, s->block);
        s->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64)
        SHA_BLOCK(s, p);
    memcpy(s->block, p, n);
    s->used = (unsigned)n;
}

static void sha_final(Sha256 *s, char hex[65]) {
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    uint64_t keep = s->length;
    sha_update(s, &pad, 1);
    while (s->used != 56)
        sha_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    s->length = keep;
    for (int i = 0; i < 8; i++)
        snprintf(hex + i * 8, 9, "%08x", s->h[i]);
}

#define CHUNK (64u * 1024u)
#define BUFFER (16u * CHUNK)

static int fail(const char *stage, int error, const char *message) {
    printf("FAIL %s %d %s\n", stage, error, message);
    return 1;
}

static int all_zero(const uint8_t *p, size_t n) {
    const uint64_t *w = (const uint64_t *)p;
    for (size_t i = 0; i < n / 8; i++)
        if (w[i])
            return 0;
    for (size_t i = n & ~(size_t)7; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static ssize_t read_full(int fd, uint8_t *p, size_t n) {
    size_t at = 0;
    while (at < n) {
        ssize_t r = read(fd, p + at, n - at);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        at += (size_t)r;
    }
    return (ssize_t)at;
}

static int write_full(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return 0;
        p += w, n -= (size_t)w;
    }
    return 1;
}

static int hash_fd(int fd, uint8_t *buffer, char hex[65], uint64_t *length) {
    Sha256 s;
    sha_init(&s);
    *length = 0;
    for (;;) {
        ssize_t n = read_full(fd, buffer, BUFFER);
        if (n < 0)
            return 0;
        if (n == 0)
            break;
        sha_update(&s, buffer, (size_t)n);
        *length += (uint64_t)n;
    }
    sha_final(&s, hex);
    return 1;
}

static int hash_file(const char *path) {
    uint8_t *buffer = malloc(BUFFER);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (!buffer || fd < 0)
        return fail("open", errno, path);
    char hex[65];
    uint64_t length;
    int ok = hash_fd(fd, buffer, hex, &length);
    int error = errno;
    close(fd);
    free(buffer);
    if (!ok)
        return fail("read", error, path);
    printf("OK %llu 0 %s\n", (unsigned long long)length, hex);
    return 0;
}

static int copy_file(const char *source, const char *destination) {
    char part[4096];
    if (snprintf(part, sizeof(part), "%s.part", destination) >= (int)sizeof(part))
        return fail("path", ENAMETOOLONG, destination);
    if (!strcmp(source, destination))
        return fail("path", EINVAL, "source and destination are the same file");
    uint8_t *buffer = malloc(BUFFER);
    if (!buffer)
        return fail("memory", ENOMEM, "buffer");
    int in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return fail("open-source", errno, source);
    struct stat st;
    if (fstat(in, &st) || !S_ISREG(st.st_mode))
        return fail("stat-source", errno ? errno : EINVAL, source);
    unlink(part);
    int out = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0660);
    if (out < 0)
        return fail("open-destination", errno, part);
    Sha256 s;
    sha_init(&s);
    uint64_t logical = 0, written = 0;
    for (;;) {
        ssize_t n = read_full(in, buffer, BUFFER);
        if (n < 0) {
            int e = errno;
            unlink(part);
            return fail("read-source", e, source);
        }
        if (n == 0)
            break;
        sha_update(&s, buffer, (size_t)n);
        for (size_t at = 0; at < (size_t)n; at += CHUNK) {
            size_t len = (size_t)n - at < CHUNK ? (size_t)n - at : CHUNK;
            if (all_zero(buffer + at, len))
                continue;
            if (lseek(out, (off_t)(logical + at), SEEK_SET) < 0 || !write_full(out, buffer + at, len)) {
                int e = errno;
                unlink(part);
                return fail("write", e, part);
            }
            written += len;
        }
        logical += (uint64_t)n;
    }
    close(in);
    if ((uint64_t)st.st_size != logical) {
        unlink(part);
        return fail("source-changed", EAGAIN, "source size changed while copying; stop both apps first");
    }
    if (ftruncate(out, (off_t)logical) || fsync(out)) {
        int e = errno;
        unlink(part);
        return fail("finish", e, part);
    }
    close(out);
    char expected[65], actual[65];
    sha_final(&s, expected);
    int check = open(part, O_RDONLY | O_CLOEXEC);
    uint64_t length = 0;
    if (check < 0 || !hash_fd(check, buffer, actual, &length)) {
        int e = errno;
        unlink(part);
        return fail("readback", e, part);
    }
    close(check);
    free(buffer);
    if (length != logical || strcmp(expected, actual)) {
        unlink(part);
        return fail("verify", EIO, "read-back SHA-256 differs from source");
    }
    if (rename(part, destination)) {
        int e = errno;
        unlink(part);
        return fail("rename", e, destination);
    }
    printf("OK %llu %llu %s\n", (unsigned long long)logical, (unsigned long long)written, expected);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 4 && !strcmp(argv[1], "copy"))
        return copy_file(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "hash"))
        return hash_file(argv[2]);
    fprintf(stderr, "usage: %s copy SOURCE DESTINATION | hash FILE\n", argv[0]);
    return 2;
}
