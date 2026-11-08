/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../presentation.h"
#define EGS_MAX_SURFACES 128u
#define EGS_MAX_BYTES (128u*1024u*1024u)
enum { EGS_RGBA=1,EGS_BGRA=2 };
typedef struct {
    uint64_t id,context,revision,bytes;
    uint32_t width,height,format;
    GLuint texture,framebuffer;
} EgsSurface;
typedef struct {
    EGLContext owner;
    uint64_t bytes,budget,cpu_uploaded,cpu_read;
    uint32_t maximum;
    EgsSurface surfaces[EGS_MAX_SURFACES];
    char error[192];
} EgsSurfacePool;
/* Dedicated transfer context, one owner thread. Guest GL state belongs in
 * separate shared contexts. Handles never cross the wire as native GL names. */
int egs_pool_open(EgsSurfacePool *,uint64_t budget);
void egs_pool_close(EgsSurfacePool *);
int egs_surface_create(EgsSurfacePool *,uint64_t context,uint64_t id,uint32_t w,uint32_t h,uint32_t format);
int egs_surface_destroy(EgsSurfacePool *,uint64_t context,uint64_t id);
const EgsSurface *egs_surface_get(EgsSurfacePool *,uint64_t context,uint64_t id);
/* Rect data contains padded rows, excluding padding after the last row.
 * Native BGRA is converted only for this dirty rectangle, never a whole frame. */
int egs_surface_upload(EgsSurfacePool *,uint64_t context,uint64_t id,uint64_t previous,uint64_t revision,
    uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t row_bytes,const uint8_t *,uint32_t size);
int egs_surface_gpu_written(EgsSurfacePool *,uint64_t context,uint64_t id,uint64_t previous,uint64_t revision);
/* Explicit CPU lock/read only. Presentation never calls this. */
int egs_surface_read(EgsSurfacePool *,uint64_t context,uint64_t id,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint8_t *,uint32_t size);

typedef struct {
    EgPresentOptions options;
    EgPresentStats stats;
    EGLContext context;
    EGLSurface surface;
    EGLNativeWindowType window;
    uint64_t generation;
    GLuint framebuffer;
    char error[192];
} EgsPresenter;
/* Direct texture -> window GPU blit, no mirror/readback or diagnostic pixels.
 * 0=presented,1=no window (texture retained),2=graphics error. */
int egs_present(EgsPresenter *,EmberGL *owner,EgsSurfacePool *,uint64_t context,uint64_t id,uint64_t serial);
void egs_present_close(EgsPresenter *,EmberGL *owner);
