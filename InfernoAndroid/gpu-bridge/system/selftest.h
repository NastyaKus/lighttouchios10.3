/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
/* A guest-shaped EGP2 client run against the persistent system GPU host:
 * the full-panel scenario of host-test.c, but through authentication, the
 * socket, the Kotlin endpoint, JNI and the vendor driver of this phone.
 * It does not involve iOS and does not show that iOS is accelerated. */
typedef struct {
    int passed;
    char stage[48];      /* first failing step, or "done" */
    char renderer[256];  /* host GL_RENDERER as reported in HELLO */
    uint32_t calls, readback_bytes, presents, presented_to_window;
    uint32_t frames;     /* timed draw+present round trips */
    double auth_ms, setup_ms, readback_ms, frame_ms_mean, frame_ms_p95, frame_ms_max;
} EgsSelfTest;
/* Takes ownership of fd, a connected stream socket. frames<=600. After the
 * timed presents and before any context is destroyed, hold(user) is called
 * (when not NULL) so the caller can capture the window while the last frame
 * is still the host's; a zero return fails the run at stage "hold". */
int egs_selftest_run(int fd, const uint8_t secret[32], const uint8_t run[16], uint32_t frames,
                     int (*hold)(void *), void *user, EgsSelfTest *);
/* key=value lines, stable for parsing by GpuLab and tests. */
int egs_selftest_format(const EgsSelfTest *, char *out, size_t size);
