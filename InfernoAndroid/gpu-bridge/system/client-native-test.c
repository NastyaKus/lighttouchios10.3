/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native C client -> authenticated EGP2 -> Kotlin -> JNI -> actual host ES3.
 * Explicit 6MB readback verifies transport+execution, not SpringBoard. */
#include "client.h"
#include "commands.h"
#include "../stream.h"
#include <stdarg.h>
#ifdef EGPU_GUEST_FREESTANDING
struct address {
    uint8_t length, family;
    uint16_t port;
    uint32_t ipv4;
    uint8_t pad[8];
};
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
static Egp2Client gpu;
static int call(int readback, uint8_t **out, uint32_t *size, unsigned words, ...) {
    uint8_t p[128];
    if (words > 32)
        return 0;
    va_list args;
    va_start(args, words);
    for (unsigned i = 0; i < words; i++)
        egp2_put32(p + i * 4, va_arg(args, unsigned));
    va_end(args);
    return egp2_client_call(&gpu, 1, p, words * 4, NULL, 0, readback, out, size);
}
static int command(unsigned words, ...) {
    uint8_t p[128];
    if (words > 32)
        return 0;
    va_list args;
    va_start(args, words);
    for (unsigned i = 0; i < words; i++)
        egp2_put32(p + i * 4, va_arg(args, unsigned));
    va_end(args);
    uint8_t *result = NULL;
    uint32_t size = 0;
    int ok = egp2_client_call(&gpu, 1, p, words * 4, NULL, 0, 0, &result, &size);
    free(result);
    return ok && size == 0;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    unsigned port = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9' || port > 6553)
            return 2;
        port = port * 10 + (unsigned)(*p - '0');
    }
    if (!port || port > 65535)
        return 2;
    uint8_t capability[48];
    unsigned at = 0;
    while (at < sizeof(capability)) {
        ssize_t n = read(0, capability + at, sizeof(capability) - at);
        if (n <= 0)
            return 3;
        at += (unsigned)n;
    }
    int fd = socket(2, 1, 0);
    if (fd < 0)
        return 4;
#ifdef EGPU_GUEST_FREESTANDING
    struct address address = {sizeof(address), 2, (uint16_t)((port >> 8) | (port << 8)), 0x0202000a, {0}};
#else
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
    if (connect(fd, (const void *)&address, sizeof(address)))
        return 4;
    int one = 1;
    setsockopt(fd, 6, 1, &one, sizeof(one));
    int ok = egp2_client_auth(&gpu, fd, capability, capability + 32);
    memset(capability, 0, sizeof(capability));
    if (!ok)
        return 5;
    if (!egp2_client_create(&gpu, 1, 0, 828, 1792))
        return 6;
    if (!command(6, EGS_SURFACE_CREATE, 77u, 0u, 828u, 1792u, 1u) ||
        !command(4, EGS_OBJECT_CREATE, EGS_FRAMEBUFFER, 8u, 0u) || !command(3, EGS_FRAMEBUFFER_BIND, 0x8d40u, 8u) ||
        !command(5, EGS_SURFACE_ATTACH, 77u, 0u, 0x8d40u, 0x8ce0u) ||
        !command(5, EGS_CLEAR_COLOR, 0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u) ||
        !command(2, EGS_CLEAR, 0x4000u))
        return 7;
    uint8_t *pixels = NULL;
    uint32_t size = 0;
    if (!call(1, &pixels, &size, 7, EGS_SURFACE_READ, 77u, 0u, 0u, 0u, 828u, 1792u) || size != 828u * 1792u * 4)
        return 8;
    for (unsigned i = 0; i < size; i += 4)
        if (pixels[i] != 64 || pixels[i + 1] != 128 || pixels[i + 2] != 191 || pixels[i + 3] != 255) {
            free(pixels);
            return 9;
        }
    free(pixels);
    uint32_t status = 0;
    if (!egp2_client_present(&gpu, 1, 77, 1, &status) || status != 1)
        return 10;
    if (!egp2_client_destroy(&gpu, 1))
        return 11;
    dprintf(1, "EGP2 native backend PASS renderer=%s readback_bytes=%u window=absent system_iOS_GPU=not_verified\n",
            gpu.renderer, size);
    egp2_client_close(&gpu);
    return 0;
}
