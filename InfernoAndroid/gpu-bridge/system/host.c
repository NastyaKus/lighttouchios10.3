/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>

#define BGRA 0x80e1u
#define LEVELS 14u
#define ATTACHMENTS 10u
#define DEFAULT_SURFACE(id) (UINT64_C(0x8000000000000000) | (id))
typedef struct Object Object;
typedef struct {
    Object *buffer;
    uint64_t offset;
    uint32_t size, type, stride, divisor;
    int enabled, normalized;
} Attribute;
typedef struct {
    Object *object;
    uint64_t surface;
} Attachment;
struct Object {
    uint32_t id, kind, group, local;
    GLuint name;
    int deleted, defined;
    uint64_t bytes, size, surface, levels[LEVELS];
    uint32_t widths[LEVELS], heights[LEVELS], base_level, max_level;
    uint8_t *mirror;
    Attribute attributes[EGS_MAX_ATTRIBS];
    Object *element, *shaders[8];
    Attachment attachments[ATTACHMENTS];
};
typedef struct {
    uint32_t id, group, width, height;
    EGLContext egl;
    EGLSurface pbuffer;
    GLuint default_fbo, default_vao;
    GLenum error;
    Object default_array, *array, *array_buffer, *renderbuffer, *program, *read_fbo, *draw_fbo;
    Object *textures[EGS_MAX_TEXTURE_UNITS];
    uint32_t unit;
    GLuint stream_vbo, stream_ibo; /* EGS_DRAW_CLIENT staging, never exposed */
} Context;
struct EgsHost {
    EmberGL transfer;
    EgsSurfacePool pool;
    EgsPresenter presenter;
    Context contexts[EGS_MAX_CONTEXTS];
    Object objects[EGS_MAX_OBJECTS];
    uint64_t bytes;
    uint32_t next_group;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image;
    char error[192];
};
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static float real(const uint8_t *p) {
    uint32_t x = u32(p);
    float f;
    memcpy(&f, &x, 4);
    return f;
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static int fail(EgsHost *h, const char *message) {
    snprintf(h->error, sizeof(h->error), "%s", message);
    return 0;
}
const char *egs_host_error(EgsHost *h) { return h->error; }
const char *egs_host_renderer(EgsHost *h) { return h->transfer.renderer; }
uint32_t egs_host_texture_limit(EgsHost *h) { return h->pool.maximum; }
static Context *context(EgsHost *h, uint32_t id) {
    for (unsigned i = 0; i < EGS_MAX_CONTEXTS; i++)
        if (h->contexts[i].id == id)
            return h->contexts + i;
    return NULL;
}
static int make_transfer(EgsHost *h) {
    return eglMakeCurrent(h->transfer.display, h->transfer.surface, h->transfer.surface, h->transfer.context);
}
static int enter(EgsHost *h, Context *c) {
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!sync || !eglMakeCurrent(h->transfer.display, c->pbuffer, c->pbuffer, c->egl))
        return 0;
    glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(sync);
    return 1;
}
/* Explicit cross-context ordering: command completion is GPU queued, not an
 * implicit CPU readback. Every following transfer/presentation observes it. */
static int leave(EgsHost *h) {
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!sync || !make_transfer(h))
        return fail(h, "Cannot synchronize guest context");
    glWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(sync);
    return 1;
}
static void gl_error(Context *c, GLenum error) {
    if (!c->error)
        c->error = error;
}
static void collect(Context *c) {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR)
        gl_error(c, e);
}
static int reply(EgsReply *r, uint32_t size) {
    r->data = size ? calloc(1, size) : NULL;
    r->size = size;
    return !size || r->data != NULL;
}
static int scalar(EgsReply *r, uint32_t value) {
    if (!reply(r, 4))
        return 0;
    put32(r->data, value);
    return 1;
}
void egs_reply_close(EgsReply *r) {
    free(r->data);
    memset(r, 0, sizeof(*r));
}
static int local_kind(uint32_t kind) { return kind == EGS_FRAMEBUFFER || kind == EGS_VERTEX_ARRAY; }
static Object *object(EgsHost *h, Context *c, uint32_t id, uint32_t kind) {
    if (!id)
        return NULL;
    for (unsigned i = 0; i < EGS_MAX_OBJECTS; i++) {
        Object *o = h->objects + i;
        if (o->id == id && (!o->deleted || kind == EGS_SHADER || kind == EGS_PROGRAM) && o->kind == kind &&
            o->group == c->group && (!local_kind(kind) || o->local == c->id))
            return o;
    }
    return NULL;
}
static Object *bound_array(Context *c) { return c->array ? c->array : &c->default_array; }
static Object *bound_buffer(Context *c, uint32_t target) {
    return target == GL_ARRAY_BUFFER           ? c->array_buffer
           : target == GL_ELEMENT_ARRAY_BUFFER ? bound_array(c)->element
                                               : NULL;
}
static int attachment_index(uint32_t attachment) {
    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment < GL_COLOR_ATTACHMENT0 + 8)
        return (int)(attachment - GL_COLOR_ATTACHMENT0);
    return attachment == GL_DEPTH_ATTACHMENT ? 8 : attachment == GL_STENCIL_ATTACHMENT ? 9 : -1;
}
static int budget(EgsHost *h, Object *o, uint64_t bytes) {
    return bytes <= EGS_ARRAY_LIMIT && bytes <= EGS_OBJECT_BUDGET - (h->bytes - o->bytes);
}
static void charge(EgsHost *h, Object *o, uint64_t bytes) {
    h->bytes = h->bytes - o->bytes + bytes;
    o->bytes = bytes;
}
static int references(EgsHost *h, Object *target) {
    for (unsigned i = 0; i < EGS_MAX_CONTEXTS; i++) {
        Context *c = h->contexts + i;
        if (!c->id)
            continue;
        if (c->array == target || c->array_buffer == target || c->renderbuffer == target || c->program == target ||
            c->read_fbo == target || c->draw_fbo == target)
            return 1;
        if (c->default_array.element == target)
            return 1;
        for (unsigned j = 0; j < EGS_MAX_ATTRIBS; j++)
            if (c->default_array.attributes[j].buffer == target)
                return 1;
        for (unsigned j = 0; j < EGS_MAX_TEXTURE_UNITS; j++)
            if (c->textures[j] == target)
                return 1;
    }
    for (unsigned i = 0; i < EGS_MAX_OBJECTS; i++) {
        Object *o = h->objects + i;
        if (!o->id || o == target)
            continue;
        if (o->element == target)
            return 1;
        for (unsigned j = 0; j < EGS_MAX_ATTRIBS; j++)
            if (o->attributes[j].buffer == target)
                return 1;
        for (unsigned j = 0; j < ATTACHMENTS; j++)
            if (o->attachments[j].object == target)
                return 1;
        for (unsigned j = 0; j < 8; j++)
            if (o->shaders[j] == target)
                return 1;
    }
    return 0;
}
static void reap(EgsHost *h) {
    int progress;
    do {
        progress = 0;
        for (unsigned i = 0; i < EGS_MAX_OBJECTS; i++) {
            Object *o = h->objects + i;
            if (!o->id || !o->deleted || references(h, o))
                continue;
            // The native GL name was deleted ONCE. It may already have been reused by
            // the driver; deleting it again here would destroy a different object.
            glFinish();
            h->bytes -= o->bytes;
            free(o->mirror);
            memset(o, 0, sizeof(*o));
            progress = 1;
        }
    } while (progress);
}
static void native_delete(Object *o) {
    switch (o->kind) {
    case EGS_SHADER:
        glDeleteShader(o->name);
        break;
    case EGS_PROGRAM:
        glDeleteProgram(o->name);
        break;
    case EGS_BUFFER:
        glDeleteBuffers(1, &o->name);
        break;
    case EGS_TEXTURE:
        glDeleteTextures(1, &o->name);
        break;
    case EGS_RENDERBUFFER:
        glDeleteRenderbuffers(1, &o->name);
        break;
    case EGS_FRAMEBUFFER:
        glDeleteFramebuffers(1, &o->name);
        break;
    case EGS_VERTEX_ARRAY:
        glDeleteVertexArrays(1, &o->name);
        break;
    default:
        break;
    }
    o->deleted = 1;
}
static void delete_object(EgsHost *h, Context *c, Object *o) {
    if (!o)
        return;
    native_delete(o);
    if (o->kind == EGS_BUFFER) {
        if (c->array_buffer == o)
            c->array_buffer = NULL;
        Object *v = bound_array(c);
        if (v->element == o)
            v->element = NULL;
        for (unsigned i = 0; i < EGS_MAX_ATTRIBS; i++)
            if (v->attributes[i].buffer == o)
                v->attributes[i].buffer = NULL;
    }
    if (o->kind == EGS_TEXTURE)
        for (unsigned i = 0; i < EGS_MAX_TEXTURE_UNITS; i++)
            if (c->textures[i] == o) {
                c->textures[i] = NULL;
            }
    if (o->kind == EGS_RENDERBUFFER && c->renderbuffer == o)
        c->renderbuffer = NULL;
    if (o->kind == EGS_TEXTURE || o->kind == EGS_RENDERBUFFER) {
        Object *f = c->draw_fbo;
        if (f)
            for (unsigned i = 0; i < ATTACHMENTS; i++)
                if (f->attachments[i].object == o) {
                    memset(f->attachments + i, 0, sizeof(Attachment));
                }
        f = c->read_fbo;
        if (f)
            for (unsigned i = 0; i < ATTACHMENTS; i++)
                if (f->attachments[i].object == o) {
                    memset(f->attachments + i, 0, sizeof(Attachment));
                }
    }
    if (o->kind == EGS_FRAMEBUFFER) {
        if (c->draw_fbo == o) {
            c->draw_fbo = NULL;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->default_fbo);
        }
        if (c->read_fbo == o) {
            c->read_fbo = NULL;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, c->default_fbo);
        }
    }
    if (o->kind == EGS_VERTEX_ARRAY && c->array == o) {
        c->array = NULL;
        glBindVertexArray(c->default_vao);
    }
    reap(h);
}
EgsHost *egs_host_open(const EgPresentOptions *options, char error[192]) {
    EgsHost *h = calloc(1, sizeof(*h));
    if (!h) {
        snprintf(error, 192, "GPU host allocation failed");
        return NULL;
    }
    if (!ember_gl_open(&h->transfer, (EGLNativeWindowType)0, true, 1, 1) || !egs_pool_open(&h->pool, EGS_MAX_BYTES)) {
        snprintf(error, 192, "%.190s", h->transfer.error[0] ? h->transfer.error : h->pool.error);
        egs_host_close(h);
        return NULL;
    }
    if (options)
        h->presenter.options = *options;
    h->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    h->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    h->bind_image = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!h->create_image || !h->destroy_image || !h->bind_image) {
        snprintf(error, 192, "EGL image sharing unavailable");
        egs_host_close(h);
        return NULL;
    }
    h->next_group = 1;
    return h;
}
int egs_host_create(EgsHost *h, uint32_t id, uint32_t share, uint32_t width, uint32_t height) {
    if (!id || context(h, id) || !width || !height || width > h->pool.maximum || height > h->pool.maximum ||
        (uint64_t)width * height * 4 > EGS_ARRAY_LIMIT)
        return fail(h, "Invalid guest context dimensions/identity");
    Context *parent = share ? context(h, share) : NULL, *c = NULL;
    if (share && !parent)
        return fail(h, "Unknown context share group");
    for (unsigned i = 0; i < EGS_MAX_CONTEXTS; i++)
        if (!h->contexts[i].id) {
            c = h->contexts + i;
            break;
        }
    if (!c || (!parent && !h->next_group))
        return fail(h, "Context table exhausted");
    c->id = id;
    c->group = parent ? parent->group : h->next_group++;
    c->width = width;
    c->height = height;
    const EGLint attrs[] = {EGL_SURFACE_TYPE,
                            EGL_PBUFFER_BIT,
                            EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES3_BIT,
                            EGL_RED_SIZE,
                            8,
                            EGL_GREEN_SIZE,
                            8,
                            EGL_BLUE_SIZE,
                            8,
                            EGL_ALPHA_SIZE,
                            8,
                            EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!make_transfer(h) || !eglChooseConfig(h->transfer.display, attrs, &config, 1, &count) || !count)
        goto bad;
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}, pa[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    c->egl = eglCreateContext(h->transfer.display, config, h->transfer.context, ca);
    c->pbuffer = eglCreatePbufferSurface(h->transfer.display, config, pa);
    if (!c->egl || !c->pbuffer || !egs_surface_create(&h->pool, c->group, DEFAULT_SURFACE(id), width, height, EGS_RGBA))
        goto bad;
    const EgsSurface *s = egs_surface_get(&h->pool, c->group, DEFAULT_SURFACE(id));
    GLuint texture = s ? s->texture : 0;
    if (!texture || !enter(h, c))
        goto bad;
    glGenVertexArrays(1, &c->default_vao);
    glBindVertexArray(c->default_vao);
    glGenFramebuffers(1, &c->default_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c->default_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR)
        goto bad;
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    return leave(h);
bad:
    egs_host_destroy(h, id);
    return fail(h, "Cannot create native ES3 guest context");
}
int egs_host_destroy(EgsHost *h, uint32_t id) {
    Context *c = context(h, id);
    if (!c)
        return fail(h, "Unknown context");
    uint32_t group = c->group;
    int other = 0;
    for (unsigned i = 0; i < EGS_MAX_CONTEXTS; i++)
        if (h->contexts[i].id && h->contexts + i != c && h->contexts[i].group == group)
            other = 1;
    if (c->egl && c->pbuffer && enter(h, c)) {
        for (unsigned i = 0; i < EGS_MAX_OBJECTS; i++) {
            Object *o = h->objects + i;
            if (o->id && !o->deleted && o->group == group && ((!local_kind(o->kind) && !other) || o->local == id))
                native_delete(o);
        }
        glDeleteFramebuffers(1, &c->default_fbo);
        glDeleteVertexArrays(1, &c->default_vao);
        if (c->stream_vbo)
            glDeleteBuffers(1, &c->stream_vbo);
        if (c->stream_ibo)
            glDeleteBuffers(1, &c->stream_ibo);
        glFinish();
    }
    make_transfer(h);
    if (c->pbuffer)
        eglDestroySurface(h->transfer.display, c->pbuffer);
    if (c->egl)
        eglDestroyContext(h->transfer.display, c->egl);
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < EGS_MAX_OBJECTS; i++) {
        Object *o = h->objects + i;
        if (o->id && o->group == group && ((!local_kind(o->kind) && !other) || o->local == id)) {
            h->bytes -= o->bytes;
            free(o->mirror);
            memset(o, 0, sizeof(*o));
        }
    }
    // Default target belongs to this context even if its share group survives.
    if (egs_surface_get(&h->pool, group, DEFAULT_SURFACE(id)))
        egs_surface_destroy(&h->pool, group, DEFAULT_SURFACE(id));
    if (!other)
        for (unsigned i = 0; i < EGS_MAX_SURFACES; i++)
            if (h->pool.surfaces[i].id && h->pool.surfaces[i].context == group)
                egs_surface_destroy(&h->pool, group, h->pool.surfaces[i].id);
    reap(h);
    return 1;
}
void egs_host_close(EgsHost *h) {
    if (!h)
        return;
    for (unsigned i = 0; i < EGS_MAX_CONTEXTS; i++)
        if (h->contexts[i].id)
            egs_host_destroy(h, h->contexts[i].id);
    if (h->transfer.context && make_transfer(h)) {
        egs_present_close(&h->presenter, &h->transfer);
        egs_pool_close(&h->pool);
    }
    ember_gl_close(&h->transfer);
    free(h);
}
int egs_host_present(EgsHost *h, uint32_t cid, uint64_t surface, uint64_t serial, int *status) {
    Context *c = context(h, cid);
    if (!c || !make_transfer(h))
        return fail(h, "Invalid presentation context");
    *status = egs_present(&h->presenter, &h->transfer, &h->pool, c->group, surface, serial);
    return *status != 2 || fail(h, h->presenter.error);
}
static int batchable(uint32_t);
static int shape(const uint8_t *p, uint32_t n, int nested) {
    if (!p || n < 4 || n > EGS_MAX_COMMAND_BYTES)
        return 0;
    uint32_t op = u32(p), expected = 0;
    switch (op) {
    case EGS_BATCH: {
        if (nested || n < 8 || u32(p + 4) > 1024)
            return 0;
        uint32_t at = 8;
        for (uint32_t i = 0; i < u32(p + 4); i++) {
            if (n - at < 12)
                return 0;
            uint32_t len = u32(p + at);
            at += 12;
            if (len > n - at || !shape(p + at, len, 1) || !batchable(u32(p + at)))
                return 0;
            at += len;
        }
        return at == n;
    }
    case EGS_GET_ERROR:
    case EGS_FINISH:
    case EGS_FLUSH:
        expected = 4;
        break;
    case EGS_GET_STRING:
    case EGS_GET_INTEGER:
    case EGS_SHADER_COMPILE:
    case EGS_PROGRAM_LINK:
    case EGS_PROGRAM_USE:
    case EGS_PROGRAM_VALIDATE:
    case EGS_TEXTURE_ACTIVE:
    case EGS_TEXTURE_MIPMAP:
    case EGS_FRAMEBUFFER_STATUS:
    case EGS_READ_BUFFER:
    case EGS_VERTEX_ARRAY_BIND:
    case EGS_CLEAR:
    case EGS_DEPTH_FUNC:
    case EGS_DEPTH_MASK:
    case EGS_CULL_FACE:
    case EGS_FRONT_FACE:
    case EGS_LINE_WIDTH:
    case EGS_CLEAR_DEPTH:
    case EGS_CLEAR_STENCIL:
        expected = 8;
        break;
    case EGS_SURFACE_DESTROY:
    case EGS_OBJECT_DELETE:
    case EGS_SHADER_SOURCE:
    case EGS_SHADER_PARAMETER:
    case EGS_SHADER_LOG:
    case EGS_SHADER_ATTACH:
    case EGS_PROGRAM_PARAMETER:
    case EGS_PROGRAM_LOG:
    case EGS_SHADER_DETACH:
    case EGS_BUFFER_BIND:
    case EGS_TEXTURE_BIND:
    case EGS_RENDERBUFFER_BIND:
    case EGS_FRAMEBUFFER_BIND:
    case EGS_ATTRIB_ENABLE:
    case EGS_ATTRIB_DIVISOR:
    case EGS_ENABLE:
    case EGS_BLEND_EQUATION:
    case EGS_DEPTH_RANGE:
    case EGS_STENCIL_MASK:
    case EGS_POLYGON_OFFSET:
    case EGS_TEXTURE_PARAMETER_QUERY:
        expected = 12;
        break;
    case EGS_SURFACE_TEXTURE:
    case EGS_OBJECT_CREATE:
    case EGS_UNIFORM_FV:
    case EGS_UNIFORM_IV:
    case EGS_TEXTURE_PARAMETER:
        expected = 16;
        break;
    case EGS_SURFACE_ATTACH:
    case EGS_UNIFORM_MATRIX:
    case EGS_FRAMEBUFFER_RENDERBUFFER:
    case EGS_DRAW_ARRAYS:
    case EGS_VIEWPORT:
    case EGS_SCISSOR:
    case EGS_CLEAR_COLOR:
    case EGS_COLOR_MASK:
    case EGS_BLEND_FUNC:
    case EGS_BLEND_COLOR:
    case EGS_STENCIL_FUNC:
    case EGS_STENCIL_OP:
        expected = 20;
        break;
    case EGS_SURFACE_CREATE:
    case EGS_BUFFER_DATA:
    case EGS_BUFFER_SUB_DATA:
    case EGS_RENDERBUFFER_STORAGE:
    case EGS_FRAMEBUFFER_TEXTURE:
    case EGS_ATTRIB_VALUE:
        expected = 24;
        break;
    case EGS_SURFACE_READ:
    case EGS_SURFACE_COMMIT:
    case EGS_DRAW_ELEMENTS:
    case EGS_READ_PIXELS:
        expected = 28;
        break;
    case EGS_ATTRIB_POINTER:
        expected = 32;
        break;
    case EGS_TEXTURE_SUB_IMAGE:
        expected = 36;
        break;
    case EGS_TEXTURE_IMAGE:
        expected = 40;
        break;
    case EGS_BLIT_FRAMEBUFFER:
        expected = 44;
        break;
    case EGS_SURFACE_UPLOAD:
        expected = 48;
        break;
    case EGS_ATTRIB_LOCATION:
    case EGS_ATTRIB_QUERY:
    case EGS_UNIFORM_QUERY: {
        uint32_t prefix = op == EGS_ATTRIB_LOCATION ? 16 : 12;
        if (n < prefix)
            return 0;
        uint32_t len = u32(p + prefix - 4);
        return len && len <= 1024 && n == prefix + len && !memchr(p + prefix, 0, len);
    }
    case EGS_DRAW_BUFFERS:
        return n >= 8 && u32(p + 4) <= 8 && n == 8 + u32(p + 4) * 4;
    case EGS_DRAW_CLIENT:
        return n >= 32 && u32(p + 28) >= 1 && u32(p + 28) <= EGS_MAX_ATTRIBS && n == 32 + u32(p + 28) * 24;
    default:
        return 0;
    }
    return n == expected;
}
static int surface_command(uint32_t op) { return op >= EGS_SURFACE_CREATE && op <= EGS_SURFACE_COMMIT; }
static int uses_blob(uint32_t op) {
    return op == EGS_SURFACE_UPLOAD || op == EGS_SHADER_SOURCE || op == EGS_UNIFORM_FV || op == EGS_UNIFORM_IV ||
           op == EGS_UNIFORM_MATRIX || op == EGS_BUFFER_DATA || op == EGS_BUFFER_SUB_DATA || op == EGS_TEXTURE_IMAGE ||
           op == EGS_TEXTURE_SUB_IMAGE || op == EGS_DRAW_CLIENT;
}
static int surface_referenced(EgsHost *h, uint64_t id) {
    for (unsigned i = 0; i < EGS_MAX_OBJECTS; i++)
        if (h->objects[i].id) {
            Object *o = h->objects + i;
            if (o->surface == id)
                return 1;
            for (unsigned j = 0; j < ATTACHMENTS; j++)
                if (o->attachments[j].surface == id)
                    return 1;
        }
    return 0;
}
static int surface_call(EgsHost *h, Context *c, const uint8_t *p, const uint8_t *upload, uint32_t size, EgsReply *out) {
    uint32_t op = u32(p);
    uint64_t id = u64(p + 4);
    if (!id || id > INT64_MAX)
        return fail(h, "Invalid public IOSurface identity");
    const EgsSurface *s = egs_surface_get(&h->pool, c->group, id);
    switch (op) {
    case EGS_SURFACE_CREATE:
        if (!egs_surface_create(&h->pool, c->group, id, u32(p + 12), u32(p + 16), u32(p + 20)))
            return fail(h, h->pool.error);
        break;
    case EGS_SURFACE_DESTROY:
        if (surface_referenced(h, id))
            return fail(h, "IOSurface still attached or aliased");
        if (!egs_surface_destroy(&h->pool, c->group, id))
            return fail(h, h->pool.error);
        break;
    case EGS_SURFACE_UPLOAD:
        if (!egs_surface_upload(&h->pool, c->group, id, u64(p + 12), u64(p + 20), u32(p + 28), u32(p + 32), u32(p + 36),
                                u32(p + 40), u32(p + 44), upload, size))
            return fail(h, h->pool.error);
        break;
    case EGS_SURFACE_READ: {
        uint64_t bytes = (uint64_t)u32(p + 20) * u32(p + 24) * 4;
        if (!s || !bytes || bytes > EGS_ARRAY_LIMIT || !reply(out, (uint32_t)bytes))
            return fail(h, "Invalid IOSurface CPU read size");
        if (!egs_surface_read(&h->pool, c->group, id, u32(p + 12), u32(p + 16), u32(p + 20), u32(p + 24), out->data,
                              out->size))
            return fail(h, h->pool.error);
        break;
    }
    case EGS_SURFACE_COMMIT:
        if (!egs_surface_gpu_written(&h->pool, c->group, id, u64(p + 12), u64(p + 20)))
            return fail(h, h->pool.error);
        break;
    case EGS_SURFACE_TEXTURE: {
        Object *o = c->textures[c->unit];
        if (!s || u32(p + 12) != GL_TEXTURE_2D || !o)
            return fail(h, "IOSurface texture view requires a bound virtual texture");
        const EGLint attributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLImageKHR image = h->create_image(h->transfer.display, h->transfer.context, EGL_GL_TEXTURE_2D_KHR,
                                            (EGLClientBuffer)(uintptr_t)s->texture, attributes);
        if (image == EGL_NO_IMAGE_KHR)
            return fail(h, "Cannot create shared IOSurface EGL image");
        if (!enter(h, c)) {
            h->destroy_image(h->transfer.display, image);
            return fail(h, "Cannot attach IOSurface texture");
        }
        h->bind_image(GL_TEXTURE_2D, (GLeglImageOES)image);
        GLenum error = glGetError();
        h->destroy_image(h->transfer.display, image);
        if (error) {
            make_transfer(h);
            return fail(h, "Cannot bind IOSurface EGL image");
        }
        if (o->bytes) {
            glFinish();
            charge(h, o, 0);
        }
        memset(o->levels, 0, sizeof(o->levels));
        memset(o->widths, 0, sizeof(o->widths));
        memset(o->heights, 0, sizeof(o->heights));
        o->surface = id;
        collect(c);
        return leave(h);
    }
    case EGS_SURFACE_ATTACH: {
        uint32_t target = u32(p + 12), attach = u32(p + 16);
        int slot = attachment_index(attach);
        Object *f = target == GL_READ_FRAMEBUFFER ? c->read_fbo : c->draw_fbo;
        if (!s || !f || slot < 0 ||
            (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER))
            return fail(h, "Invalid IOSurface framebuffer attachment");
        if (!enter(h, c))
            return fail(h, "Cannot attach IOSurface framebuffer");
        glFramebufferTexture2D(target, attach, GL_TEXTURE_2D, s->texture, 0);
        if (glGetError() != GL_NO_ERROR) {
            make_transfer(h);
            return fail(h, "IOSurface framebuffer attachment failed");
        }
        f->attachments[slot] = (Attachment){NULL, id};
        reap(h);
        return leave(h);
    }
    default:
        return fail(h, "Unsupported IOSurface operation");
    }
    return 1;
}
static uint32_t type_bytes(uint32_t type) {
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        return 2;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
    case GL_FIXED:
        return 4;
    default:
        return 0;
    }
}
static uint32_t pixel_bytes(uint32_t format, uint32_t type) {
    uint32_t components = format == GL_RGBA || format == BGRA ? 4
                          : format == GL_RGB                  ? 3
                          : format == GL_RG                   ? 2
                          : format == GL_RED                  ? 1
                                                              : 0;
    if (!components)
        return 0;
    if (type == GL_UNSIGNED_SHORT_5_6_5)
        return format == GL_RGB ? 2 : 0;
    if (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
        return format == GL_RGBA ? 2 : 0;
    return (type == GL_UNSIGNED_BYTE || type == GL_HALF_FLOAT || type == GL_FLOAT) ? components * type_bytes(type) : 0;
}
/* Whether the last element a draw reads (vertex `maximum`, or the last
 * instance for a divisor) lies inside `length` bytes starting at `offset`. */
static int span_safe(uint64_t length, uint64_t offset, uint32_t components, uint32_t type, uint32_t stride,
                     uint32_t divisor, uint32_t maximum, uint32_t instances) {
    uint64_t bytes = (uint64_t)components * type_bytes(type), step = stride ? stride : bytes;
    uint64_t index = divisor ? (instances - 1) / divisor : maximum;
    return bytes && offset <= length && bytes <= length - offset && index <= (length - offset - bytes) / step;
}
/* `skip` marks attributes the caller supplies itself (client arrays). */
static int draw_safe_except(Context *c, uint32_t maximum, uint32_t instances, const uint8_t skip[EGS_MAX_ATTRIBS]) {
    Object *v = bound_array(c);
    for (unsigned i = 0; i < EGS_MAX_ATTRIBS; i++)
        if (v->attributes[i].enabled && !(skip && skip[i])) {
            Attribute *a = v->attributes + i;
            if (!a->buffer || !span_safe(a->buffer->size, a->offset, a->size, a->type, a->stride, a->divisor, maximum,
                                         instances))
                return 0;
        }
    return 1;
}
static int draw_safe(Context *c, uint32_t maximum, uint32_t instances) {
    return draw_safe_except(c, maximum, instances, NULL);
}
/* Highest non-restart index, or 0 for none. */
static uint32_t index_maximum(const uint8_t *p, uint32_t count, uint32_t unit) {
    uint32_t maximum = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *idx = p + (uint64_t)i * unit;
        uint32_t v = idx[0];
        if (unit >= 2)
            v |= (uint32_t)idx[1] << 8;
        if (unit == 4)
            v |= (uint32_t)idx[2] << 16 | (uint32_t)idx[3] << 24;
        // ES3 fixed primitive-restart indices do not reference a vertex.
        if ((unit == 1 && v == UINT8_MAX) || (unit == 2 && v == UINT16_MAX) || (unit == 4 && v == UINT32_MAX))
            continue;
        if (v > maximum)
            maximum = v;
    }
    return maximum;
}
static uint32_t index_unit(uint32_t type) {
    return type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
}
/* EGS_DRAW_CLIENT: validate everything, stage into this context's private
 * stream buffers, draw, then put back the bindings/pointers the guest set. */
static void draw_client(Context *c, const uint8_t *p, const uint8_t *upload, uint32_t size) {
    uint32_t mode = u32(p + 4), first = u32(p + 8), count = u32(p + 12), type = u32(p + 16), at = u32(p + 20),
             instances = u32(p + 24), k = u32(p + 28), unit = index_unit(type), maximum = 0;
    if (!count || !instances)
        return;
    if (mode > GL_TRIANGLE_FAN || count > INT_MAX || instances > INT_MAX || !upload || size > EGS_CLIENT_DRAW_LIMIT ||
        (type && !unit) || (!type && at) || (type && first)) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    if (type) {
        if (at % unit || at > size || (uint64_t)count * unit > size - at) {
            gl_error(c, GL_INVALID_OPERATION);
            return;
        }
        maximum = index_maximum(upload + at, count, unit);
    } else {
        if (first > INT_MAX || count > INT_MAX - first) {
            gl_error(c, GL_INVALID_OPERATION);
            return;
        }
        maximum = first + count - 1;
    }
    Object *v = bound_array(c);
    uint8_t mine[EGS_MAX_ATTRIBS] = {0};
    for (uint32_t i = 0; i < k; i++) {
        const uint8_t *a = p + 32 + i * 24;
        uint32_t index = u32(a), components = u32(a + 4), kind = u32(a + 8), normalized = u32(a + 12),
                 stride = u32(a + 16), offset = u32(a + 20);
        if (index >= EGS_MAX_ATTRIBS || mine[index] || components < 1 || components > 4 || !type_bytes(kind) ||
            normalized > 1 || stride > 2048) {
            gl_error(c, GL_INVALID_VALUE);
            return;
        }
        if (!span_safe(size, offset, components, kind, stride, v->attributes[index].divisor, maximum, instances)) {
            gl_error(c, GL_INVALID_OPERATION);
            return;
        }
        mine[index] = 1;
    }
    if (!draw_safe_except(c, maximum, instances, mine)) {
        gl_error(c, GL_INVALID_OPERATION);
        return;
    }
    if (!c->stream_vbo)
        glGenBuffers(1, &c->stream_vbo);
    if (type && !c->stream_ibo)
        glGenBuffers(1, &c->stream_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, c->stream_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, upload, GL_STREAM_DRAW);
    for (uint32_t i = 0; i < k; i++) {
        const uint8_t *a = p + 32 + i * 24;
        glVertexAttribPointer(u32(a), (GLint)u32(a + 4), u32(a + 8), u32(a + 12) != 0, (GLsizei)u32(a + 16),
                              (const void *)(uintptr_t)u32(a + 20));
    }
    if (type) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->stream_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((uint64_t)count * unit), upload + at, GL_STREAM_DRAW);
        glDrawElementsInstanced(mode, (GLsizei)count, type, NULL, (GLsizei)instances);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, v->element ? v->element->name : 0); // retained names stay bound
    } else
        glDrawArraysInstanced(mode, (GLint)first, (GLsizei)count, (GLsizei)instances);
    GLenum error = glGetError();
    // Client pointers are per draw: restore what the guest specified before.
    for (uint32_t i = 0; i < EGS_MAX_ATTRIBS; i++)
        if (mine[i]) {
            Attribute *t = v->attributes + i;
            if (!t->buffer)
                continue; // never specified; draw_safe refuses it until it is
            glBindBuffer(GL_ARRAY_BUFFER, t->buffer->name);
            glVertexAttribPointer(i, (GLint)t->size, t->type, t->normalized != 0, (GLsizei)t->stride,
                                  (const void *)(uintptr_t)t->offset);
        }
    glBindBuffer(GL_ARRAY_BUFFER, c->array_buffer ? c->array_buffer->name : 0);
    if (error)
        gl_error(c, error);
}
static int integer(EgsHost *h, Context *c, uint32_t name, EgsReply *out) {
    GLint v[4] = {0};
    unsigned count = 1;
    Object *o = NULL;
    switch (name) {
    case GL_CURRENT_PROGRAM:
        o = c->program;
        break;
    case GL_ARRAY_BUFFER_BINDING:
        o = c->array_buffer;
        break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
        o = bound_array(c)->element;
        break;
    case GL_TEXTURE_BINDING_2D:
        o = c->textures[c->unit];
        break;
    case GL_DRAW_FRAMEBUFFER_BINDING:
        o = c->draw_fbo;
        break;
    case GL_READ_FRAMEBUFFER_BINDING:
        o = c->read_fbo;
        break;
    case GL_RENDERBUFFER_BINDING:
        o = c->renderbuffer;
        break;
    case GL_VERTEX_ARRAY_BINDING:
        o = c->array;
        break;
    case GL_VIEWPORT:
    case GL_SCISSOR_BOX:
        count = 4;
        glGetIntegerv(name, v);
        goto result;
    case GL_MAX_VIEWPORT_DIMS:
        count = 2;
        glGetIntegerv(name, v);
        goto result;
    case GL_MAX_TEXTURE_SIZE:
        v[0] = (GLint)h->pool.maximum;
        goto result;
    case GL_MAX_VERTEX_ATTRIBS:
        v[0] = EGS_MAX_ATTRIBS;
        goto result;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
    case GL_MAX_TEXTURE_IMAGE_UNITS:
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
        v[0] = EGS_MAX_TEXTURE_UNITS;
        goto result;
    case GL_ACTIVE_TEXTURE:
    case GL_MAX_RENDERBUFFER_SIZE:
    case GL_MAX_VERTEX_UNIFORM_VECTORS:
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
    case GL_MAX_VARYING_VECTORS:
    case GL_MAX_DRAW_BUFFERS:
    case GL_MAX_COLOR_ATTACHMENTS:
    case GL_PACK_ALIGNMENT:
    case GL_UNPACK_ALIGNMENT:
    case GL_SAMPLES:
    case GL_SAMPLE_BUFFERS:
    case GL_DEPTH_FUNC:
    case GL_STENCIL_FUNC:
    case GL_BLEND_SRC_RGB:
    case GL_BLEND_DST_RGB:
    case GL_BLEND_SRC_ALPHA:
    case GL_BLEND_DST_ALPHA:
    case GL_CULL_FACE_MODE:
    case GL_FRONT_FACE:
        glGetIntegerv(name, v);
        goto result;
    default:
        gl_error(c, GL_INVALID_ENUM);
        return scalar(out, 0);
    }
    v[0] = o ? (GLint)o->id : 0;
result:
    if (!reply(out, count * 4))
        return fail(h, "Integer reply allocation");
    for (unsigned i = 0; i < count; i++)
        put32(out->data + i * 4, (uint32_t)v[i]);
    return 1;
}
static int create_object(EgsHost *h, Context *c, uint32_t kind, uint32_t id, uint32_t arg) {
    if (kind < EGS_SHADER || kind > EGS_VERTEX_ARRAY) {
        gl_error(c, GL_INVALID_ENUM);
        return 1;
    }
    if (!id || object(h, c, id, kind)) {
        gl_error(c, GL_INVALID_VALUE);
        return 1;
    }
    Object *o = NULL;
    for (unsigned i = 0; i < EGS_MAX_OBJECTS; i++)
        if (!h->objects[i].id) {
            o = h->objects + i;
            break;
        }
    if (!o) {
        gl_error(c, GL_OUT_OF_MEMORY);
        return 1;
    }
    GLuint native = 0;
    switch (kind) {
    case EGS_SHADER:
        if (arg != GL_VERTEX_SHADER && arg != GL_FRAGMENT_SHADER) {
            gl_error(c, GL_INVALID_ENUM);
            return 1;
        }
        native = glCreateShader(arg);
        break;
    case EGS_PROGRAM:
        native = glCreateProgram();
        break;
    case EGS_BUFFER:
        glGenBuffers(1, &native);
        break;
    case EGS_TEXTURE:
        glGenTextures(1, &native);
        break;
    case EGS_RENDERBUFFER:
        glGenRenderbuffers(1, &native);
        break;
    case EGS_FRAMEBUFFER:
        glGenFramebuffers(1, &native);
        break;
    case EGS_VERTEX_ARRAY:
        glGenVertexArrays(1, &native);
        break;
    default:
        break;
    }
    if (native) {
        o->id = id;
        o->kind = kind;
        o->name = native;
        o->group = c->group;
        o->local = local_kind(kind) ? c->id : 0;
        o->max_level = LEVELS - 1;
    }
    return 1;
}
static int shader_log(EgsHost *h, Context *c, Object *o, uint32_t maximum, int program, EgsReply *out) {
    if (!o || !maximum || maximum > 1 << 20) {
        gl_error(c, GL_INVALID_VALUE);
        return scalar(out, 0);
    }
    if (!reply(out, maximum + 4))
        return fail(h, "Shader log allocation");
    GLsizei length = 0;
    if (program)
        glGetProgramInfoLog(o->name, (GLsizei)maximum, &length, (char *)out->data + 4);
    else
        glGetShaderInfoLog(o->name, (GLsizei)maximum, &length, (char *)out->data + 4);
    if (length < 0 || (uint32_t)length >= maximum)
        return fail(h, "Invalid native shader log length");
    put32(out->data, (uint32_t)length);
    out->size = (uint32_t)length + 4;
    return 1;
}
#define U(i) u32(p + 4 + (i) * 4)
#define F(i) real(p + 4 + (i) * 4)
static int gl_call(EgsHost *h, Context *c, const uint8_t *p, uint32_t n, const uint8_t *upload, uint32_t size,
                   EgsReply *out) {
    uint32_t op = u32(p);
    Object *a, *b;
    GLint value = 0;
    char text[1025];
    switch (op) {
    case EGS_GET_ERROR:
        value = (GLint)c->error;
        c->error = 0;
        return scalar(out, (uint32_t)value);
    case EGS_GET_INTEGER:
        return integer(h, c, U(0), out);
    case EGS_GET_STRING: {
        // Extension advertisement belongs to the guest frontend. Do not advertise
        // host extensions for which it has no dispatch/marshalling implementation.
        if (U(0) != GL_VENDOR && U(0) != GL_RENDERER && U(0) != GL_VERSION && U(0) != GL_SHADING_LANGUAGE_VERSION) {
            gl_error(c, GL_INVALID_ENUM);
            return reply(out, 0);
        }
        const char *s = (const char *)glGetString(U(0));
        if (!s)
            return reply(out, 0);
        size_t len = strlen(s);
        if (len > 4096 || !reply(out, (uint32_t)len))
            return fail(h, "Native GL string bounds");
        memcpy(out->data, s, len);
        return 1;
    }
    case EGS_FINISH:
        glFinish();
        break;
    case EGS_FLUSH:
        glFlush();
        break;
    case EGS_OBJECT_CREATE:
        return create_object(h, c, U(0), U(1), U(2));
    case EGS_OBJECT_DELETE:
        a = object(h, c, U(1), U(0));
        if (a)
            delete_object(h, c, a);
        break;
    case EGS_SHADER_SOURCE:
        a = object(h, c, U(0), EGS_SHADER);
        if (!a) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        if (size != U(1) || size > 1 << 20 || !upload)
            return fail(h, "Shader source upload bounds");
        if (!budget(h, a, size)) {
            gl_error(c, GL_OUT_OF_MEMORY);
            break;
        }
        {
            GLint length = (GLint)size;
            const char *source = (const char *)upload;
            glShaderSource(a->name, 1, &source, &length);
        }
        {
            GLenum error = glGetError();
            if (error)
                gl_error(c, error);
            else
                charge(h, a, size);
        }
        break;
    case EGS_SHADER_COMPILE:
        a = object(h, c, U(0), EGS_SHADER);
        if (!a) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glCompileShader(a->name);
        break;
    case EGS_SHADER_PARAMETER:
        a = object(h, c, U(0), EGS_SHADER);
        if (!a) {
            gl_error(c, GL_INVALID_VALUE);
            return scalar(out, 0);
        }
        if (U(1) != GL_SHADER_TYPE && U(1) != GL_DELETE_STATUS && U(1) != GL_COMPILE_STATUS &&
            U(1) != GL_INFO_LOG_LENGTH && U(1) != GL_SHADER_SOURCE_LENGTH) {
            gl_error(c, GL_INVALID_ENUM);
            return scalar(out, 0);
        }
        glGetShaderiv(a->name, U(1), &value);
        return scalar(out, (uint32_t)value);
    case EGS_SHADER_LOG:
        return shader_log(h, c, object(h, c, U(0), EGS_SHADER), U(1), 0, out);
    case EGS_SHADER_ATTACH:
    case EGS_SHADER_DETACH:
        a = object(h, c, U(0), EGS_PROGRAM);
        b = object(h, c, U(1), EGS_SHADER);
        if (!a || !b) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        if (op == EGS_SHADER_ATTACH) {
            for (unsigned i = 0; i < 8; i++)
                if (a->shaders[i] == b) {
                    gl_error(c, GL_INVALID_OPERATION);
                    return 1;
                }
            unsigned slot = 0;
            while (slot < 8 && a->shaders[slot])
                slot++;
            if (slot == 8) {
                gl_error(c, GL_OUT_OF_MEMORY);
                break;
            }
            glAttachShader(a->name, b->name);
            if (glGetError() != GL_NO_ERROR) {
                gl_error(c, GL_INVALID_OPERATION);
                break;
            }
            a->shaders[slot] = b;
        } else {
            unsigned i = 0;
            while (i < 8 && a->shaders[i] != b)
                i++;
            if (i == 8) {
                gl_error(c, GL_INVALID_OPERATION);
                break;
            }
            glDetachShader(a->name, b->name);
            a->shaders[i] = NULL;
            reap(h);
        }
        break;
    case EGS_ATTRIB_LOCATION:
    case EGS_ATTRIB_QUERY:
    case EGS_UNIFORM_QUERY: {
        a = object(h, c, U(0), EGS_PROGRAM);
        if (!a) {
            gl_error(c, GL_INVALID_VALUE);
            return op == EGS_ATTRIB_LOCATION ? 1 : scalar(out, UINT32_MAX);
        }
        unsigned prefix = op == EGS_ATTRIB_LOCATION ? 16 : 12, length = u32(p + prefix - 4);
        memcpy(text, p + prefix, length);
        text[length] = 0;
        if (op == EGS_ATTRIB_LOCATION) {
            if (U(1) >= EGS_MAX_ATTRIBS) {
                gl_error(c, GL_INVALID_VALUE);
                break;
            }
            glBindAttribLocation(a->name, U(1), text);
        } else
            return scalar(out, (uint32_t)(op == EGS_ATTRIB_QUERY ? glGetAttribLocation(a->name, text)
                                                                 : glGetUniformLocation(a->name, text)));
        break;
    }
    case EGS_PROGRAM_LINK:
    case EGS_PROGRAM_VALIDATE:
        a = object(h, c, U(0), EGS_PROGRAM);
        if (!a) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        if (op == EGS_PROGRAM_LINK)
            glLinkProgram(a->name);
        else
            glValidateProgram(a->name);
        break;
    case EGS_PROGRAM_PARAMETER:
        a = object(h, c, U(0), EGS_PROGRAM);
        if (!a) {
            gl_error(c, GL_INVALID_VALUE);
            return scalar(out, 0);
        }
        if (U(1) != GL_DELETE_STATUS && U(1) != GL_LINK_STATUS && U(1) != GL_VALIDATE_STATUS &&
            U(1) != GL_INFO_LOG_LENGTH && U(1) != GL_ATTACHED_SHADERS && U(1) != GL_ACTIVE_ATTRIBUTES &&
            U(1) != GL_ACTIVE_ATTRIBUTE_MAX_LENGTH && U(1) != GL_ACTIVE_UNIFORMS &&
            U(1) != GL_ACTIVE_UNIFORM_MAX_LENGTH) {
            gl_error(c, GL_INVALID_ENUM);
            return scalar(out, 0);
        }
        glGetProgramiv(a->name, U(1), &value);
        return scalar(out, (uint32_t)value);
    case EGS_PROGRAM_LOG:
        return shader_log(h, c, object(h, c, U(0), EGS_PROGRAM), U(1), 1, out);
    case EGS_PROGRAM_USE:
        a = object(h, c, U(0), EGS_PROGRAM);
        if (U(0) && !a) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glUseProgram(a ? a->name : 0);
        if (glGetError() != GL_NO_ERROR) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        c->program = a;
        reap(h);
        break;
    case EGS_UNIFORM_FV:
    case EGS_UNIFORM_IV:
    case EGS_UNIFORM_MATRIX: {
        uint32_t components = U(1), count = U(2);
        uint64_t words = (uint64_t)components * count;
        if (op == EGS_UNIFORM_MATRIX)
            words *= components;
        if (components < 1 || components > 4 || (op == EGS_UNIFORM_MATRIX && (components < 2 || U(3))) ||
            words * 4 != size || size > EGS_ARRAY_LIMIT || (size && !upload))
            return fail(h, "Invalid uniform array/matrix encoding");
        if (!size)
            break;
        // malloc alignment, explicit LE conversion, no unaligned native array casts.
        uint32_t *array = malloc(size);
        if (!array) {
            gl_error(c, GL_OUT_OF_MEMORY);
            break;
        }
        for (uint32_t i = 0; i < size / 4; i++)
            array[i] = u32(upload + i * 4);
        GLint location = (GLint)U(0);
        if (op == EGS_UNIFORM_FV)
            switch (components) {
            case 1:
                glUniform1fv(location, count, (GLfloat *)array);
                break;
            case 2:
                glUniform2fv(location, count, (GLfloat *)array);
                break;
            case 3:
                glUniform3fv(location, count, (GLfloat *)array);
                break;
            case 4:
                glUniform4fv(location, count, (GLfloat *)array);
                break;
            }
        else if (op == EGS_UNIFORM_IV)
            switch (components) {
            case 1:
                glUniform1iv(location, count, (GLint *)array);
                break;
            case 2:
                glUniform2iv(location, count, (GLint *)array);
                break;
            case 3:
                glUniform3iv(location, count, (GLint *)array);
                break;
            case 4:
                glUniform4iv(location, count, (GLint *)array);
                break;
            }
        else
            switch (components) {
            case 2:
                glUniformMatrix2fv(location, count, GL_FALSE, (GLfloat *)array);
                break;
            case 3:
                glUniformMatrix3fv(location, count, GL_FALSE, (GLfloat *)array);
                break;
            case 4:
                glUniformMatrix4fv(location, count, GL_FALSE, (GLfloat *)array);
                break;
            }
        free(array);
        break;
    }
    case EGS_BUFFER_BIND:
        if (U(0) != GL_ARRAY_BUFFER && U(0) != GL_ELEMENT_ARRAY_BUFFER) {
            gl_error(c, GL_INVALID_ENUM);
            break;
        }
        a = object(h, c, U(1), EGS_BUFFER);
        if (U(1) && !a) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        glBindBuffer(U(0), a ? a->name : 0);
        if (U(0) == GL_ARRAY_BUFFER)
            c->array_buffer = a;
        else
            bound_array(c)->element = a;
        reap(h);
        break;
    case EGS_BUFFER_DATA: {
        a = bound_buffer(c, U(0));
        uint64_t bytes = u64(p + 8);
        if (!a) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        if (bytes > EGS_ARRAY_LIMIT || U(4) > 1 || (U(4) ? size != bytes : size != 0))
            return fail(h, "Invalid buffer data upload");
        if (!budget(h, a, bytes * 2)) {
            gl_error(c, GL_OUT_OF_MEMORY);
            break;
        }
        uint8_t *mirror = bytes ? malloc((size_t)bytes) : NULL;
        if (bytes && !mirror) {
            gl_error(c, GL_OUT_OF_MEMORY);
            break;
        }
        if (bytes) {
            if (U(4))
                memcpy(mirror, upload, (size_t)bytes);
            else
                memset(mirror, 0, (size_t)bytes);
        }
        glBufferData(U(0), (GLsizeiptr)bytes, U(4) ? upload : NULL, U(3));
        GLenum error = glGetError();
        if (error) {
            free(mirror);
            gl_error(c, error);
            break;
        }
        free(a->mirror);
        a->mirror = mirror;
        a->size = bytes;
        a->defined = U(4) != 0;
        charge(h, a, bytes * 2);
        break;
    }
    case EGS_BUFFER_SUB_DATA: {
        a = bound_buffer(c, U(0));
        uint64_t offset = u64(p + 8), bytes = u64(p + 16);
        if (!a) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        if (bytes != size || offset > a->size || bytes > a->size - offset || (size && !upload))
            return fail(h, "Buffer subdata range/upload");
        glBufferSubData(U(0), (GLintptr)offset, (GLsizeiptr)bytes, upload);
        GLenum error = glGetError();
        if (error) {
            gl_error(c, error);
            break;
        }
        if (bytes)
            memcpy(a->mirror + offset, upload, size);
        if (!offset && bytes == a->size)
            a->defined = 1;
        break;
    }
    case EGS_TEXTURE_ACTIVE:
        if (U(0) < GL_TEXTURE0 || U(0) >= GL_TEXTURE0 + EGS_MAX_TEXTURE_UNITS) {
            gl_error(c, GL_INVALID_ENUM);
            break;
        }
        glActiveTexture(U(0));
        c->unit = U(0) - GL_TEXTURE0;
        break;
    case EGS_TEXTURE_BIND:
        if (U(0) != GL_TEXTURE_2D) {
            gl_error(c, GL_INVALID_ENUM);
            break;
        }
        a = object(h, c, U(1), EGS_TEXTURE);
        if (U(1) && !a) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glBindTexture(GL_TEXTURE_2D, a ? a->name : 0);
        c->textures[c->unit] = a;
        reap(h);
        break;
    case EGS_TEXTURE_PARAMETER:
        a = c->textures[c->unit];
        if (U(0) != GL_TEXTURE_2D || !a) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        if ((U(1) == GL_TEXTURE_BASE_LEVEL || U(1) == GL_TEXTURE_MAX_LEVEL) && (GLint)U(2) < 0) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        if (U(1) == GL_TEXTURE_BASE_LEVEL && U(2) >= LEVELS) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glTexParameteri(U(0), U(1), (GLint)U(2));
        {
            GLenum error = glGetError();
            if (error) {
                gl_error(c, error);
                break;
            }
        }
        if (U(1) == GL_TEXTURE_BASE_LEVEL)
            a->base_level = U(2);
        if (U(1) == GL_TEXTURE_MAX_LEVEL)
            a->max_level = U(2) < LEVELS ? U(2) : LEVELS - 1;
        break;
    case EGS_TEXTURE_PARAMETER_QUERY:
        if (U(0) != GL_TEXTURE_2D || !c->textures[c->unit]) {
            gl_error(c, GL_INVALID_OPERATION);
            return scalar(out, 0);
        }
        switch (U(1)) {
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER:
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_IMMUTABLE_FORMAT:
        case GL_TEXTURE_IMMUTABLE_LEVELS:
            break;
        default:
            gl_error(c, GL_INVALID_ENUM);
            return scalar(out, 0);
        }
        glGetTexParameteriv(GL_TEXTURE_2D, U(1), &value);
        return scalar(out, (uint32_t)value);
    case EGS_TEXTURE_IMAGE:
    case EGS_TEXTURE_SUB_IMAGE: {
        a = c->textures[c->unit];
        if (!a || a->surface) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        uint32_t w = U(3), height = U(4), format = U(6), type = U(7), level = U(1), internal = U(2),
                 has = op == EGS_TEXTURE_IMAGE ? U(8) : 1;
        if (op == EGS_TEXTURE_SUB_IMAGE) {
            w = U(4);
            height = U(5);
            format = U(6);
            type = U(7);
            has = 1;
        }
        uint32_t pixel = pixel_bytes(format, type);
        uint64_t bytes = (uint64_t)w * height * pixel;
        if (U(0) != GL_TEXTURE_2D || !pixel || level >= LEVELS || w > h->pool.maximum || height > h->pool.maximum ||
            bytes > EGS_ARRAY_LIMIT || has > 1 || (has ? size != bytes : size != 0))
            return fail(h, "Invalid texture upload encoding");
        uint64_t charge_bytes = 0;
        if (op == EGS_TEXTURE_IMAGE) {
            if (U(5)) {
                gl_error(c, GL_INVALID_VALUE);
                break;
            }
            // Conservative RGBA-equivalent storage accounting, including all levels.
            unsigned bpp = 16;
            switch (internal) {
            case GL_RGBA:
            case GL_RGB:
            case GL_RG:
            case GL_RED:
            case BGRA:
                bpp = type == GL_FLOAT ? 16 : type == GL_HALF_FLOAT ? 8 : 4;
                break;
            case GL_RGBA8:
            case GL_RGB8:
            case GL_RG8:
            case GL_R8:
            case GL_RGB565:
            case GL_RGB5_A1:
            case GL_RGBA4:
            case GL_RGB10_A2:
            case GL_SRGB8:
            case GL_SRGB8_ALPHA8:
            case GL_R16F:
            case GL_R32F:
            case GL_RG16F:
                bpp = 4;
                break;
            case GL_RGBA16F:
            case GL_RGB16F:
            case GL_RG32F:
                bpp = 8;
                break;
            default:
                break;
            }
            uint64_t storage = (uint64_t)w * height * bpp;
            charge_bytes = a->bytes - a->levels[level] + storage;
            if (!budget(h, a, charge_bytes)) {
                gl_error(c, GL_OUT_OF_MEMORY);
                break;
            }
        }
        uint8_t *converted = NULL;
        const uint8_t *pixels = has ? upload : NULL;
        if (format == BGRA) {
            if (type != GL_UNSIGNED_BYTE)
                return fail(h, "Unsupported BGRA type");
            format = GL_RGBA;
            if (internal == BGRA)
                internal = GL_RGBA;
            if (pixels && bytes) {
                converted = malloc((size_t)bytes);
                if (!converted) {
                    gl_error(c, GL_OUT_OF_MEMORY);
                    break;
                }
                for (uint32_t i = 0; i < size; i += 4) {
                    converted[i] = pixels[i + 2];
                    converted[i + 1] = pixels[i + 1];
                    converted[i + 2] = pixels[i];
                    converted[i + 3] = pixels[i + 3];
                }
                pixels = converted;
            }
        }
        GLint alignment;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (op == EGS_TEXTURE_IMAGE)
            glTexImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)internal, (GLsizei)w, (GLsizei)height, 0, format, type,
                         pixels);
        else
            glTexSubImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)U(2), (GLint)U(3), (GLsizei)w, (GLsizei)height, format,
                            type, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        free(converted);
        GLenum error = glGetError();
        if (error) {
            gl_error(c, error);
            break;
        }
        if (op == EGS_TEXTURE_IMAGE) {
            a->levels[level] = charge_bytes - a->bytes + a->levels[level];
            a->widths[level] = w;
            a->heights[level] = height;
            charge(h, a, charge_bytes);
        }
        break;
    }
    case EGS_TEXTURE_MIPMAP: {
        a = c->textures[c->unit];
        if (U(0) != GL_TEXTURE_2D || !a || a->surface) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        unsigned base = a->base_level;
        uint32_t w = a->widths[base], height = a->heights[base];
        if (!w || !height) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        uint64_t bpp = a->levels[base] / ((uint64_t)w * height), bytes = a->bytes;
        unsigned last = base;
        for (unsigned i = base + 1; i <= a->max_level && (w > 1 || height > 1); i++) {
            w = w > 1 ? w / 2 : 1;
            height = height > 1 ? height / 2 : 1;
            bytes = bytes - a->levels[i] + (uint64_t)w * height * bpp;
            last = i;
        }
        if (!budget(h, a, bytes)) {
            gl_error(c, GL_OUT_OF_MEMORY);
            break;
        }
        glGenerateMipmap(GL_TEXTURE_2D);
        GLenum error = glGetError();
        if (error) {
            gl_error(c, error);
            break;
        }
        w = a->widths[base];
        height = a->heights[base];
        for (unsigned i = base + 1; i <= last; i++) {
            w = w > 1 ? w / 2 : 1;
            height = height > 1 ? height / 2 : 1;
            a->widths[i] = w;
            a->heights[i] = height;
            a->levels[i] = (uint64_t)w * height * bpp;
        }
        charge(h, a, bytes);
        break;
    }
    case EGS_RENDERBUFFER_BIND:
        if (U(0) != GL_RENDERBUFFER) {
            gl_error(c, GL_INVALID_ENUM);
            break;
        }
        a = object(h, c, U(1), EGS_RENDERBUFFER);
        if (U(1) && !a) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glBindRenderbuffer(GL_RENDERBUFFER, a ? a->name : 0);
        c->renderbuffer = a;
        reap(h);
        break;
    case EGS_RENDERBUFFER_STORAGE: {
        a = c->renderbuffer;
        uint32_t w = U(2), height = U(3), samples = U(4);
        uint64_t bytes = (uint64_t)w * height * 8 * (samples ? samples : 1);
        if (!a) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        if (U(0) != GL_RENDERBUFFER || w > h->pool.maximum || height > h->pool.maximum || samples > 8 ||
            !budget(h, a, bytes)) {
            gl_error(c, GL_OUT_OF_MEMORY);
            break;
        }
        if (samples)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, U(1), w, height);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, U(1), w, height);
        GLenum error = glGetError();
        if (error) {
            gl_error(c, error);
            break;
        }
        charge(h, a, bytes);
        break;
    }
    case EGS_FRAMEBUFFER_BIND:
        if (U(0) != GL_FRAMEBUFFER && U(0) != GL_READ_FRAMEBUFFER && U(0) != GL_DRAW_FRAMEBUFFER) {
            gl_error(c, GL_INVALID_ENUM);
            break;
        }
        a = object(h, c, U(1), EGS_FRAMEBUFFER);
        if (U(1) && !a) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glBindFramebuffer(U(0), a ? a->name : c->default_fbo);
        if (U(0) != GL_READ_FRAMEBUFFER)
            c->draw_fbo = a;
        if (U(0) != GL_DRAW_FRAMEBUFFER)
            c->read_fbo = a;
        reap(h);
        break;
    case EGS_FRAMEBUFFER_TEXTURE:
    case EGS_FRAMEBUFFER_RENDERBUFFER: {
        a = U(0) == GL_READ_FRAMEBUFFER ? c->read_fbo : c->draw_fbo;
        int slot = attachment_index(U(1));
        uint32_t kind = op == EGS_FRAMEBUFFER_TEXTURE ? EGS_TEXTURE : EGS_RENDERBUFFER;
        b = object(h, c, U(3), kind);
        if (!a || slot < 0 || (U(3) && !b)) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        if (op == EGS_FRAMEBUFFER_TEXTURE)
            glFramebufferTexture2D(U(0), U(1), U(2), b ? b->name : 0, (GLint)U(4));
        else
            glFramebufferRenderbuffer(U(0), U(1), U(2), b ? b->name : 0);
        GLenum error = glGetError();
        if (error) {
            gl_error(c, error);
            break;
        }
        a->attachments[slot] = (Attachment){b, 0};
        reap(h);
        break;
    }
    case EGS_FRAMEBUFFER_STATUS:
        return scalar(out, glCheckFramebufferStatus(U(0)));
    case EGS_DRAW_BUFFERS: {
        GLenum buffers[8];
        for (unsigned i = 0; i < U(0); i++)
            buffers[i] = U(i + 1);
        glDrawBuffers((GLsizei)U(0), buffers);
        break;
    }
    case EGS_READ_BUFFER:
        glReadBuffer(U(0));
        break;
    case EGS_BLIT_FRAMEBUFFER:
        glBlitFramebuffer((GLint)U(0), (GLint)U(1), (GLint)U(2), (GLint)U(3), (GLint)U(4), (GLint)U(5), (GLint)U(6),
                          (GLint)U(7), U(8), U(9));
        break;
    case EGS_VERTEX_ARRAY_BIND:
        a = object(h, c, U(0), EGS_VERTEX_ARRAY);
        if (U(0) && !a) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        glBindVertexArray(a ? a->name : c->default_vao);
        c->array = a;
        reap(h);
        break;
    case EGS_ATTRIB_POINTER: {
        uint32_t index = U(0), components = U(1), type = U(2), stride = U(4);
        uint64_t offset = u64(p + 24);
        a = c->array_buffer;
        if (index >= EGS_MAX_ATTRIBS || components < 1 || components > 4 || !type_bytes(type) || U(3) > 1 ||
            stride > 2048 || !a || offset > a->size) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glVertexAttribPointer(index, (GLint)components, type, U(3) != 0, (GLsizei)stride, (void *)(uintptr_t)offset);
        GLenum error = glGetError();
        if (error) {
            gl_error(c, error);
            break;
        }
        Attribute *v = bound_array(c)->attributes + index;
        v->buffer = a;
        v->size = components;
        v->type = type;
        v->stride = stride;
        v->offset = offset;
        v->normalized = U(3) != 0;
        reap(h);
        break;
    }
    case EGS_ATTRIB_ENABLE:
        if (U(0) >= EGS_MAX_ATTRIBS || U(1) > 1) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        if (U(1))
            glEnableVertexAttribArray(U(0));
        else
            glDisableVertexAttribArray(U(0));
        bound_array(c)->attributes[U(0)].enabled = U(1) != 0;
        break;
    case EGS_ATTRIB_DIVISOR:
        if (U(0) >= EGS_MAX_ATTRIBS) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glVertexAttribDivisor(U(0), U(1));
        bound_array(c)->attributes[U(0)].divisor = U(1);
        break;
    case EGS_ATTRIB_VALUE:
        if (U(0) >= EGS_MAX_ATTRIBS) {
            gl_error(c, GL_INVALID_VALUE);
            break;
        }
        glVertexAttrib4f(U(0), F(1), F(2), F(3), F(4));
        break;
    case EGS_DRAW_ARRAYS: {
        uint32_t first = U(1), count = U(2), instances = U(3);
        if (!count || !instances)
            break;
        if (first > INT_MAX || count > INT_MAX - first || instances > INT_MAX ||
            !draw_safe(c, first + count - 1, instances)) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        glDrawArraysInstanced(U(0), (GLint)first, (GLsizei)count, (GLsizei)instances);
        break;
    }
    case EGS_DRAW_ELEMENTS: {
        Object *indices = bound_array(c)->element;
        uint32_t count = U(1), type = U(2), instances = U(5),
                 unit = type == GL_UNSIGNED_BYTE    ? 1
                        : type == GL_UNSIGNED_SHORT ? 2
                        : type == GL_UNSIGNED_INT   ? 4
                                                    : 0;
        uint64_t offset = u64(p + 16), bytes = (uint64_t)count * unit;
        if (!count || !instances)
            break;
        if (!unit || !indices || !indices->defined || count > INT_MAX || instances > INT_MAX || offset % unit ||
            offset > indices->size || bytes > indices->size - offset) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        uint32_t maximum = index_maximum(indices->mirror + offset, count, unit);
        if (!draw_safe(c, maximum, instances)) {
            gl_error(c, GL_INVALID_OPERATION);
            break;
        }
        glDrawElementsInstanced(U(0), (GLsizei)count, type, (void *)(uintptr_t)offset, (GLsizei)instances);
        break;
    }
    case EGS_DRAW_CLIENT:
        draw_client(c, p, upload, size);
        break;
    case EGS_VIEWPORT:
        glViewport((GLint)U(0), (GLint)U(1), (GLsizei)U(2), (GLsizei)U(3));
        break;
    case EGS_SCISSOR:
        glScissor((GLint)U(0), (GLint)U(1), (GLsizei)U(2), (GLsizei)U(3));
        break;
    case EGS_ENABLE:
        if (U(1) > 1)
            return fail(h, "Invalid enable flag");
        if (U(0) != GL_BLEND && U(0) != GL_SCISSOR_TEST && U(0) != GL_DEPTH_TEST && U(0) != GL_STENCIL_TEST &&
            U(0) != GL_CULL_FACE && U(0) != GL_DITHER && U(0) != GL_POLYGON_OFFSET_FILL &&
            U(0) != GL_SAMPLE_ALPHA_TO_COVERAGE && U(0) != GL_SAMPLE_COVERAGE && U(0) != GL_RASTERIZER_DISCARD &&
            U(0) != GL_PRIMITIVE_RESTART_FIXED_INDEX) {
            gl_error(c, GL_INVALID_ENUM);
            break;
        }
        if (U(1))
            glEnable(U(0));
        else
            glDisable(U(0));
        break;
    case EGS_CLEAR_COLOR:
        glClearColor(F(0), F(1), F(2), F(3));
        break;
    case EGS_CLEAR:
        glClear(U(0));
        break;
    case EGS_COLOR_MASK:
        glColorMask(U(0) != 0, U(1) != 0, U(2) != 0, U(3) != 0);
        break;
    case EGS_BLEND_FUNC:
        glBlendFuncSeparate(U(0), U(1), U(2), U(3));
        break;
    case EGS_BLEND_EQUATION:
        glBlendEquationSeparate(U(0), U(1));
        break;
    case EGS_BLEND_COLOR:
        glBlendColor(F(0), F(1), F(2), F(3));
        break;
    case EGS_DEPTH_FUNC:
        glDepthFunc(U(0));
        break;
    case EGS_DEPTH_MASK:
        glDepthMask(U(0) != 0);
        break;
    case EGS_DEPTH_RANGE:
        glDepthRangef(F(0), F(1));
        break;
    case EGS_STENCIL_FUNC:
        glStencilFuncSeparate(U(0), U(1), (GLint)U(2), U(3));
        break;
    case EGS_STENCIL_OP:
        glStencilOpSeparate(U(0), U(1), U(2), U(3));
        break;
    case EGS_STENCIL_MASK:
        glStencilMaskSeparate(U(0), U(1));
        break;
    case EGS_CULL_FACE:
        glCullFace(U(0));
        break;
    case EGS_FRONT_FACE:
        glFrontFace(U(0));
        break;
    case EGS_LINE_WIDTH:
        glLineWidth(F(0));
        break;
    case EGS_POLYGON_OFFSET:
        glPolygonOffset(F(0), F(1));
        break;
    case EGS_CLEAR_DEPTH:
        glClearDepthf(F(0));
        break;
    case EGS_CLEAR_STENCIL:
        glClearStencil((GLint)U(0));
        break;
    case EGS_READ_PIXELS: {
        uint64_t bytes = (uint64_t)U(2) * U(3) * 4;
        if (U(4) != GL_RGBA || U(5) != GL_UNSIGNED_BYTE || !bytes || bytes > EGS_ARRAY_LIMIT ||
            !reply(out, (uint32_t)bytes))
            return fail(h, "Unsupported/bounded CPU readback");
        GLint alignment;
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels((GLint)U(0), (GLint)U(1), (GLsizei)U(2), (GLsizei)U(3), GL_RGBA, GL_UNSIGNED_BYTE, out->data);
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        {
            GLenum error = glGetError();
            if (error) {
                gl_error(c, error);
                egs_reply_close(out);
            }
        }
        break;
    }
    default:
        (void)n;
        return fail(h, "Unsupported GLES operation");
    }
    return 1;
}
#undef U
#undef F
static int batchable(uint32_t op) {
    if (surface_command(op) || op == EGS_BATCH || op == EGS_GET_ERROR || op == EGS_GET_STRING ||
        op == EGS_GET_INTEGER || op == EGS_SHADER_PARAMETER || op == EGS_SHADER_LOG || op == EGS_PROGRAM_PARAMETER ||
        op == EGS_PROGRAM_LOG || op == EGS_ATTRIB_QUERY || op == EGS_UNIFORM_QUERY || op == EGS_FRAMEBUFFER_STATUS ||
        op == EGS_READ_PIXELS || op == EGS_TEXTURE_PARAMETER_QUERY)
        return 0;
    return 1;
}
int egs_host_call(EgsHost *h, uint32_t cid, const uint8_t *p, uint32_t n, const uint8_t *upload, uint32_t size,
                  EgsReply *out) {
    memset(out, 0, sizeof(*out));
    Context *c = context(h, cid);
    if (!c || !shape(p, n, 0) || size > EGS_ARRAY_LIMIT || (size && !upload)) {
        snprintf(h->error, sizeof(h->error), "Malformed/unsupported GLES command: context=%u opcode=%u bytes=%u", cid,
                 p && n >= 4 ? u32(p) : 0, n);
        return 0;
    }
    uint32_t op = u32(p);
    int ok;
    if (op == EGS_BATCH) {
        uint32_t at = 8;
        // Validate the ENTIRE framing and all upload slices before touching GL.
        for (uint32_t i = 0; i < u32(p + 4); i++) {
            uint32_t len = u32(p + at), offset = u32(p + at + 4), bytes = u32(p + at + 8);
            at += 12;
            if (!batchable(u32(p + at)) || offset > size || bytes > size - offset ||
                (!uses_blob(u32(p + at)) && (offset || bytes)))
                return fail(h, "Invalid GLES batch slice/query");
            at += len;
        }
        if (!enter(h, c))
            return fail(h, "Cannot activate ES3 context");
        at = 8;
        ok = 1;
        for (uint32_t i = 0; i < u32(p + 4); i++) {
            uint32_t len = u32(p + at), offset = u32(p + at + 4), bytes = u32(p + at + 8);
            at += 12;
            EgsReply result = {0};
            ok = gl_call(h, c, p + at, len, bytes ? upload + offset : NULL, bytes, &result);
            collect(c);
            at += len;
            if (result.size) {
                egs_reply_close(&result);
                ok = fail(h, "GLES batch unexpectedly returned data");
            }
            if (!ok)
                break;
        }
    } else {
        if (!uses_blob(op) && size)
            return fail(h, "Unexpected GLES upload");
        if (!make_transfer(h))
            return fail(h, "Cannot activate transfer context");
        if (surface_command(op)) {
            ok = surface_call(h, c, p, upload, size, out);
            if (!ok)
                egs_reply_close(out);
            return ok;
        }
        if (!enter(h, c))
            return fail(h, "Cannot activate ES3 context");
        ok = gl_call(h, c, p, n, upload, size, out);
        collect(c);
    }
    int synchronized = leave(h);
    if (!ok || !synchronized) {
        egs_reply_close(out);
        return 0;
    }
    return 1;
}
