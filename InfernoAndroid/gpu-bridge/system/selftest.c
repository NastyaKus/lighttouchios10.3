/* SPDX-License-Identifier: GPL-3.0-or-later
 * Guest-shaped EGP2 run against the real system GPU host (see selftest.h).
 * Mirrors host-test.c: shaders, uniform matrix and sampler, indexed draw,
 * a padded BGRA IOSurface sampled through a texture view, two shared
 * contexts, an explicit full-panel readback compared pixel by pixel, then
 * timed draw+present round trips onto whatever window the host has. */
#include "selftest.h"
#include "client.h"
#include "commands.h"
#include "../stream.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#ifdef EGPU_GUEST_FREESTANDING
#error The self-test is a host-side diagnostic, not a guest program
#endif

/* GL enums by value: the client side must not depend on a GL library. */
enum {
    VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82,
    ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, STATIC_DRAW = 0x88E4, FLOAT = 0x1406,
    TEXTURE0 = 0x84C0, TEXTURE_2D = 0x0DE1, MIN_FILTER = 0x2801, NEAREST = 0x2600, LINEAR = 0x2601,
    FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5,
    TRIANGLES = 4, UNSIGNED_SHORT = 0x1403, COLOR_BUFFER_BIT = 0x4000
};
/* Pixel formats of the surface pool (gpu-bridge/system/surface.h). */
enum { SURFACE_RGBA = 1, SURFACE_BGRA = 2 };
#define PANEL_W 828u
#define PANEL_H 1792u
#define TEXEL_R 220
#define TEXEL_G 45
#define TEXEL_B 7

typedef struct {
    Egp2Client gpu;
    EgsSelfTest *r;
    int (*hold)(void *);
    void *user;
} Run;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static uint32_t bits(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}
static int fail(Run *x, const char *stage) {
    snprintf(x->r->stage, sizeof(x->r->stage), "%s", stage);
    return 0;
}
/* words... -> CALL with optional upload; result returned when wanted. */
static int vcall(Run *x, uint32_t context, const uint8_t *upload, uint32_t bytes, int readback, uint8_t **out,
                 uint32_t *size, unsigned count, va_list args) {
    uint8_t p[128];
    if (count > 32)
        return 0;
    for (unsigned i = 0; i < count; i++)
        egp2_put32(p + i * 4, va_arg(args, unsigned));
    uint8_t *result = NULL;
    uint32_t n = 0;
    int ok = egp2_client_call(&x->gpu, context, p, count * 4, upload, bytes, readback, &result, &n);
    x->r->calls++;
    if (out) {
        *out = result;
        *size = n;
    } else {
        free(result);
        ok = ok && n == 0;
    }
    return ok;
}
static int cmd(Run *x, uint32_t context, unsigned count, ...) {
    va_list a;
    va_start(a, count);
    int ok = vcall(x, context, NULL, 0, 0, NULL, NULL, count, a);
    va_end(a);
    return ok;
}
static int upload(Run *x, uint32_t context, const void *data, uint32_t bytes, unsigned count, ...) {
    va_list a;
    va_start(a, count);
    uint8_t *out = NULL;
    uint32_t n = 0;
    int ok = vcall(x, context, data, bytes, 0, &out, &n, count, a);
    va_end(a);
    free(out);
    return ok;
}
static int word(Run *x, uint32_t context, uint32_t *value, unsigned count, ...) {
    va_list a;
    va_start(a, count);
    uint8_t *out = NULL;
    uint32_t n = 0;
    int ok = vcall(x, context, NULL, 0, 0, &out, &n, count, a);
    va_end(a);
    ok = ok && n >= 4;
    if (ok)
        *value = egp2_get32(out);
    free(out);
    return ok;
}
static int shader(Run *x, uint32_t id, uint32_t type, const char *text) {
    uint32_t length = (uint32_t)strlen(text), status = 0;
    return cmd(x, 1, 4, EGS_OBJECT_CREATE, EGS_SHADER, id, type) &&
           upload(x, 1, text, length, 3, EGS_SHADER_SOURCE, id, length) && cmd(x, 1, 2, EGS_SHADER_COMPILE, id) &&
           word(x, 1, &status, 3, EGS_SHADER_PARAMETER, id, COMPILE_STATUS) && status;
}
static int uniform(Run *x, const char *name, uint32_t *location) {
    uint8_t p[64];
    uint32_t n = (uint32_t)strlen(name);
    egp2_put32(p, EGS_UNIFORM_QUERY);
    egp2_put32(p + 4, 100);
    egp2_put32(p + 8, n);
    memcpy(p + 12, name, n);
    uint8_t *out = NULL;
    uint32_t size = 0;
    int ok = egp2_client_call(&x->gpu, 1, p, n + 12, NULL, 0, 0, &out, &size) && size == 4;
    x->r->calls++;
    if (ok)
        *location = egp2_get32(out);
    free(out);
    return ok && *location != UINT32_MAX;
}
static int draw_state(Run *x, uint32_t c) {
    uint32_t status = 0;
    return cmd(x, c, 2, EGS_PROGRAM_USE, 100u) && cmd(x, c, 3, EGS_BUFFER_BIND, ARRAY_BUFFER, 200u) &&
           cmd(x, c, 8, EGS_ATTRIB_POINTER, 0u, 2u, FLOAT, 0u, 16u, 0u, 0u) &&
           cmd(x, c, 8, EGS_ATTRIB_POINTER, 1u, 2u, FLOAT, 0u, 16u, 8u, 0u) &&
           cmd(x, c, 3, EGS_ATTRIB_ENABLE, 0u, 1u) && cmd(x, c, 3, EGS_ATTRIB_ENABLE, 1u, 1u) &&
           cmd(x, c, 3, EGS_BUFFER_BIND, ELEMENT_ARRAY_BUFFER, 201u) && cmd(x, c, 2, EGS_TEXTURE_ACTIVE, TEXTURE0) &&
           cmd(x, c, 3, EGS_TEXTURE_BIND, TEXTURE_2D, 300u) && cmd(x, c, 5, EGS_VIEWPORT, 0u, 0u, PANEL_W, PANEL_H) &&
           cmd(x, c, 4, EGS_OBJECT_CREATE, EGS_FRAMEBUFFER, 400u, 0u) &&
           cmd(x, c, 3, EGS_FRAMEBUFFER_BIND, FRAMEBUFFER, 400u) &&
           cmd(x, c, 5, EGS_SURFACE_ATTACH, 700u, 0u, FRAMEBUFFER, COLOR_ATTACHMENT0) &&
           word(x, c, &status, 2, EGS_FRAMEBUFFER_STATUS, FRAMEBUFFER) && status == FRAMEBUFFER_COMPLETE;
}
static int draw(Run *x, uint32_t c) {
    uint32_t error = 1;
    return cmd(x, c, 7, EGS_DRAW_ELEMENTS, TRIANGLES, 6u, UNSIGNED_SHORT, 0u, 0u, 1u) &&
           word(x, c, &error, 1, EGS_GET_ERROR) && error == 0;
}
static int verify_panel(Run *x, uint32_t c) {
    uint8_t *pixels = NULL;
    uint32_t size = 0;
    uint8_t p[28];
    uint32_t words[7] = {EGS_SURFACE_READ, 700u, 0u, 0u, 0u, PANEL_W, PANEL_H};
    for (unsigned i = 0; i < 7; i++)
        egp2_put32(p + i * 4, words[i]);
    double started = now_ms();
    int ok = egp2_client_call(&x->gpu, c, p, sizeof(p), NULL, 0, 1, &pixels, &size);
    x->r->calls++;
    x->r->readback_ms = now_ms() - started;
    ok = ok && size == PANEL_W * PANEL_H * 4;
    for (uint32_t i = 0; ok && i < size; i += 4)
        ok = pixels[i] == TEXEL_R && pixels[i + 1] == TEXEL_G && pixels[i + 2] == TEXEL_B && pixels[i + 3] == 255;
    if (ok)
        x->r->readback_bytes = size;
    free(pixels);
    return ok;
}
static int compare(const void *a, const void *b) {
    double l = *(const double *)a, r = *(const double *)b;
    return (l > r) - (l < r);
}

static int scenario(Run *x, uint32_t frames) {
    if (!egp2_client_create(&x->gpu, 1, 0, PANEL_W, PANEL_H) || !egp2_client_create(&x->gpu, 2, 1, PANEL_W, PANEL_H))
        return fail(x, "context");
    if (!shader(x, 10, VERTEX_SHADER,
                "#version 300 es\nlayout(location=0) in vec2 p;layout(location=1) in vec2 t;uniform mat4 matrix;"
                "out vec2 uv;void main(){gl_Position=matrix*vec4(p,0,1);uv=t;}") ||
        !shader(x, 11, FRAGMENT_SHADER,
                "#version 300 es\nprecision highp float;uniform sampler2D image;in vec2 uv;out vec4 color;"
                "void main(){color=texture(image,uv);}"))
        return fail(x, "shader");
    uint32_t linked = 0, matrix = 0, image = 0;
    if (!cmd(x, 1, 4, EGS_OBJECT_CREATE, EGS_PROGRAM, 100u, 0u) || !cmd(x, 1, 3, EGS_SHADER_ATTACH, 100u, 10u) ||
        !cmd(x, 1, 3, EGS_SHADER_ATTACH, 100u, 11u) || !cmd(x, 1, 2, EGS_PROGRAM_LINK, 100u) ||
        !word(x, 1, &linked, 3, EGS_PROGRAM_PARAMETER, 100u, LINK_STATUS) || !linked ||
        !cmd(x, 1, 2, EGS_PROGRAM_USE, 100u) || !uniform(x, "matrix", &matrix) || !uniform(x, "image", &image))
        return fail(x, "program");
    uint8_t identity[64] = {0}, sampler[4] = {0};
    for (unsigned i = 0; i < 16; i++)
        egp2_put32(identity + i * 4, bits(i % 5 == 0 ? 1.f : 0.f));
    if (!upload(x, 1, identity, sizeof(identity), 5, EGS_UNIFORM_MATRIX, matrix, 4u, 1u, 0u) ||
        !upload(x, 1, sampler, sizeof(sampler), 4, EGS_UNIFORM_IV, image, 1u, 1u))
        return fail(x, "uniform");
    static const float vertices[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    uint8_t vb[sizeof(vertices)];
    for (unsigned i = 0; i < sizeof(vertices) / 4; i++)
        egp2_put32(vb + i * 4, bits(vertices[i]));
    static const uint8_t ib[] = {0, 0, 1, 0, 2, 0, 2, 0, 1, 0, 3, 0};
    if (!cmd(x, 1, 4, EGS_OBJECT_CREATE, EGS_BUFFER, 200u, 0u) || !cmd(x, 1, 3, EGS_BUFFER_BIND, ARRAY_BUFFER, 200u) ||
        !upload(x, 1, vb, sizeof(vb), 6, EGS_BUFFER_DATA, ARRAY_BUFFER, (unsigned)sizeof(vb), 0u, STATIC_DRAW, 1u) ||
        !cmd(x, 1, 4, EGS_OBJECT_CREATE, EGS_BUFFER, 201u, 0u) ||
        !cmd(x, 1, 3, EGS_BUFFER_BIND, ELEMENT_ARRAY_BUFFER, 201u) ||
        !upload(x, 1, ib, sizeof(ib), 6, EGS_BUFFER_DATA, ELEMENT_ARRAY_BUFFER, (unsigned)sizeof(ib), 0u, STATIC_DRAW,
                1u))
        return fail(x, "buffers");
    /* 17x19 BGRA with 72-byte rows (4 bytes of padding), as IOSurface lays it out. */
    uint8_t texels[19 * 72 - 4];
    memset(texels, 0x55, sizeof(texels));
    for (unsigned y = 0; y < 19; y++)
        for (unsigned xx = 0; xx < 17; xx++) {
            uint8_t *t = texels + y * 72 + xx * 4;
            t[0] = TEXEL_B, t[1] = TEXEL_G, t[2] = TEXEL_R, t[3] = 255;
        }
    uint32_t filter = 0;
    if (!cmd(x, 1, 6, EGS_SURFACE_CREATE, 600u, 0u, 17u, 19u, SURFACE_BGRA) ||
        !upload(x, 1, texels, sizeof(texels), 12, EGS_SURFACE_UPLOAD, 600u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 17u, 19u, 72u) ||
        !cmd(x, 1, 4, EGS_OBJECT_CREATE, EGS_TEXTURE, 300u, 0u) || !cmd(x, 1, 3, EGS_TEXTURE_BIND, TEXTURE_2D, 300u) ||
        !cmd(x, 1, 4, EGS_SURFACE_TEXTURE, 600u, 0u, TEXTURE_2D) ||
        !cmd(x, 1, 4, EGS_TEXTURE_PARAMETER, TEXTURE_2D, MIN_FILTER, NEAREST) ||
        /* A second view of the same storage keeps its own sampler state. */
        !cmd(x, 1, 4, EGS_OBJECT_CREATE, EGS_TEXTURE, 301u, 0u) || !cmd(x, 1, 3, EGS_TEXTURE_BIND, TEXTURE_2D, 301u) ||
        !cmd(x, 1, 4, EGS_SURFACE_TEXTURE, 600u, 0u, TEXTURE_2D) ||
        !cmd(x, 1, 4, EGS_TEXTURE_PARAMETER, TEXTURE_2D, MIN_FILTER, LINEAR) ||
        !cmd(x, 1, 3, EGS_TEXTURE_BIND, TEXTURE_2D, 300u) ||
        !word(x, 1, &filter, 3, EGS_TEXTURE_PARAMETER_QUERY, TEXTURE_2D, MIN_FILTER) || filter != NEAREST ||
        !cmd(x, 1, 3, EGS_OBJECT_DELETE, EGS_TEXTURE, 301u) ||
        !cmd(x, 1, 6, EGS_SURFACE_CREATE, 700u, 0u, PANEL_W, PANEL_H, SURFACE_RGBA))
        return fail(x, "iosurface");
    if (!draw_state(x, 1) || !draw(x, 1))
        return fail(x, "draw");
    if (!verify_panel(x, 1))
        return fail(x, "readback");
    /* The shared context resolves the same virtual program/buffers/texture. */
    if (!draw_state(x, 2) || !draw(x, 2) || !verify_panel(x, 2))
        return fail(x, "shared-context");
    if (frames > 600)
        frames = 600;
    double *times = calloc(frames ? frames : 1, sizeof(double));
    if (!times)
        return fail(x, "memory");
    for (uint32_t i = 0; i < frames; i++) {
        double started = now_ms();
        uint32_t status = 2;
        if (!cmd(x, 2, 7, EGS_DRAW_ELEMENTS, TRIANGLES, 6u, UNSIGNED_SHORT, 0u, 0u, 1u) ||
            !egp2_client_present(&x->gpu, 2, 700, i + 1, &status)) {
            free(times);
            return fail(x, "present");
        }
        times[i] = now_ms() - started;
        x->r->presents++;
        if (status == 0)
            x->r->presented_to_window++;
    }
    if (frames) {
        double sum = 0;
        for (uint32_t i = 0; i < frames; i++)
            sum += times[i];
        qsort(times, frames, sizeof(double), compare);
        x->r->frames = frames;
        x->r->frame_ms_mean = sum / frames;
        x->r->frame_ms_p95 = times[(frames * 95 + 99) / 100 - 1];
        x->r->frame_ms_max = times[frames - 1];
    }
    free(times);
    if (x->hold && !x->hold(x->user))
        return fail(x, "hold");
    if (!egp2_client_destroy(&x->gpu, 1) || !egp2_client_destroy(&x->gpu, 2))
        return fail(x, "destroy");
    return 1;
}

int egs_selftest_run(int fd, const uint8_t secret[32], const uint8_t run[16], uint32_t frames,
                     int (*hold)(void *), void *user, EgsSelfTest *r) {
    memset(r, 0, sizeof(*r));
    Run x = {.r = r, .hold = hold, .user = user};
    snprintf(r->stage, sizeof(r->stage), "auth");
    double started = now_ms();
    if (!egp2_client_auth(&x.gpu, fd, secret, run))
        return 0;
    r->auth_ms = now_ms() - started;
    strncpy(r->renderer, x.gpu.renderer, sizeof(r->renderer) - 1); /* truncation is fine for a report */
    started = now_ms();
    int ok = scenario(&x, frames);
    r->setup_ms = now_ms() - started;
    egp2_client_close(&x.gpu);
    if (ok) {
        r->passed = 1;
        snprintf(r->stage, sizeof(r->stage), "done");
    }
    return ok;
}

int egs_selftest_format(const EgsSelfTest *r, char *out, size_t size) {
    char renderer[256];
    size_t n = 0;
    for (const char *p = r->renderer; *p && n + 1 < sizeof(renderer); p++)
        renderer[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    renderer[n] = 0;
    int w = snprintf(out, size,
                     "passed=%d\nstage=%s\nrenderer=%s\ncalls=%u\nreadback_bytes=%u\npresents=%u\n"
                     "presented_to_window=%u\nframes=%u\nauth_ms=%.2f\ntotal_ms=%.2f\nreadback_ms=%.2f\n"
                     "frame_ms_mean=%.3f\nframe_ms_p95=%.3f\nframe_ms_max=%.3f\nsystem_iOS_GPU=not_verified\n",
                     r->passed, r->stage, renderer, r->calls, r->readback_bytes, r->presents, r->presented_to_window,
                     r->frames, r->auth_ms, r->setup_ms, r->readback_ms, r->frame_ms_mean, r->frame_ms_p95,
                     r->frame_ms_max);
    return w > 0 && (size_t)w < size;
}
