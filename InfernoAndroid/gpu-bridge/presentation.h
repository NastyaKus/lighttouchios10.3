/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gles_renderer.h"
typedef struct {
    uint64_t frames,window_frames,generation,surface_creates,surface_losses,last_serial;
    uint32_t pixels[4],width,height;
    uint64_t sample_pixels,cpu_upload_bytes;
} EgPresentStats;
typedef struct {
    int require_window;
    void *opaque;
    /* Returns an acquired native-window reference; release is paired by us. */
    EGLNativeWindowType (*acquire_window)(void *,uint64_t *generation);
    void (*release_window)(void *,EGLNativeWindowType);
    void (*publish)(void *,const EgPresentStats *);
} EgPresentOptions;
typedef struct {
    EgPresentOptions options;
    EgPresentStats stats;
    EGLContext context;
    EGLSurface surface;
    EGLNativeWindowType window;
    uint64_t active_generation;
    uint32_t width,height;
    GLuint read_fbo,mirror_fbo,mirror_texture;
    char error[192];
} EgPresenter;
/* Same worker as the producer, a separate shared context preserves GL state.
 * Return 0=presented, 1=no drawable, 2=graphics failure. Never uploads a CPU frame. */
int eg_present(EgPresenter *,EmberGL *owner,GLuint renderbuffer,uint32_t w,uint32_t h,uint32_t serial);
void eg_present_close(EgPresenter *,EmberGL *owner);
