/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "protocol.h"
#include "gles_renderer.h"
#include "gles-host.h"
#include "presentation.h"
typedef struct EgpuExecutor {
    EmberGL context;
    GLuint framebuffer, color, program, vao, buffer;
    uint32_t width, height, handle, next_handle;
    uint64_t commands, draws, pixels_read;
    EgGlesHost gles;
    EgPresenter presenter;
} EgpuExecutor;
/* Own worker thread/context only, never called under QEMU's BQL. */
int egpu_executor_open(EgpuExecutor *e);
void egpu_executor_close(EgpuExecutor *e);
int egpu_execute(EgpuExecutor *e,const EgpuFrame *request,uint8_t reply[EGPU_PAGE]);
