/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gles-wire.h"
#include <GLES3/gl3.h>
typedef struct { uint32_t kind;GLuint name;uint32_t bytes,width,height,attachment; } EgObject;
typedef struct { uint32_t object,offset,stride,size;int enabled; } EgAttrib;
typedef struct {
    EgObject objects[EG_MAX_OBJECTS];
    EgAttrib attributes[EG_MAX_ATTRIBS];
    GLuint vao;
    uint32_t array_buffer, texture_unit, textures[8],renderbuffer,framebuffer,present_serial;
    GLenum error;
    uint64_t calls, batches, draws, shader_compiles;
} EgGlesHost;
struct EgpuExecutor;
int eg_gles_host_command(struct EgpuExecutor *e,uint16_t op,const uint8_t *data,uint32_t n,uint8_t *out,uint32_t *out_size);
void eg_gles_host_close(EgGlesHost *g);
