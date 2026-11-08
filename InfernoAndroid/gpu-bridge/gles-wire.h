/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "protocol.h"
#define EGPU_CAP_GLES_SUBSET 16u
#define EGPU_CAP_PRESENT 32u
#define EGPU_GLES_BATCH 0x100u
#define EGPU_GLES_QUERY 0x101u
#define EGPU_GLES_PRESENT 0x102u
#define EG_MAX_OBJECTS 64u
#define EG_MAX_SOURCE 2048u
#define EG_MAX_BUFFER 2048u
#define EG_MAX_TEX_SIDE 16u
#define EG_MAX_ATTRIBS 8u
enum { EG_SHADER=1, EG_PROGRAM=2, EG_BUFFER=3, EG_TEXTURE=4, EG_RENDERBUFFER=5, EG_FRAMEBUFFER=6 };
enum {
    EG_NEW=1, EG_DELETE, EG_SOURCE, EG_COMPILE, EG_ATTACH, EG_ATTRIB_LOCATION,
    EG_LINK, EG_USE, EG_BIND_BUFFER, EG_BUFFER_DATA, EG_ATTRIB_POINTER,
    EG_ATTRIB_ENABLE, EG_VIEWPORT, EG_CLEAR_COLOR, EG_CLEAR, EG_DRAW,
    EG_ACTIVE_TEXTURE, EG_BIND_TEXTURE, EG_TEX_PARAMETER, EG_TEX_IMAGE,
    EG_UNIFORM4F, EG_UNIFORM1I, EG_BLEND_FUNC, EG_ENABLE, EG_SCISSOR,
    EG_BIND_RBO, EG_RBO_STORAGE, EG_BIND_FBO, EG_FBO_ATTACH
};
enum { EG_SHADER_IV=1, EG_PROGRAM_IV, EG_ATTRIB_QUERY, EG_UNIFORM_QUERY,
       EG_ERROR_QUERY, EG_LOG_QUERY, EG_READ_RECT, EG_INTEGER_QUERY, EG_RBO_IV, EG_FBO_STATUS };
static inline float eg_get_float(const uint8_t *p) {
    union { uint32_t u; float f; } v={.u=egpu_u32(p)};return v.f;
}
static inline void eg_put_float(uint8_t *p,float f) {
    union { uint32_t u; float f; } v={.f=f};egpu_put32(p,v.u);
}
