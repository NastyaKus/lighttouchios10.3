/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

/* All calls, including destroy, belong to the render thread. No QEMU locks. */
typedef struct {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint program, texture, vao;
    uint32_t width, height;
    bool full_upload;
    char error[192], vendor[128], renderer[192], version[128];
} EmberGL;

/* NULL window creates a pbuffer, used by the same-source host regression test. */
bool ember_gl_open(EmberGL *g, EGLNativeWindowType window, bool pbuffer, int w, int h);
void ember_gl_close(EmberGL *g);
bool ember_gl_draw(EmberGL *g, const uint8_t *bgra, size_t size,
                   uint32_t w, uint32_t h, uint32_t stride,
                   uint32_t x, uint32_t y, uint32_t dw, uint32_t dh,
                   uint64_t *uploaded);
bool ember_gl_swap(EmberGL *g);
