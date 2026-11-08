/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gles_renderer.h"
#include <stdio.h>
#include <string.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

static bool fail(EmberGL *g, const char *stage, unsigned code)
{
    snprintf(g->error, sizeof(g->error), "%s: 0x%x", stage, code);
    return false;
}

void ember_gl_close(EmberGL *g)
{
    if (g->display != EGL_NO_DISPLAY) {
        if (g->context != EGL_NO_CONTEXT &&
            eglMakeCurrent(g->display, g->surface, g->surface, g->context)) {
            glDeleteTextures(1, &g->texture);
            glDeleteProgram(g->program);
            glDeleteVertexArrays(1, &g->vao);
        }
        eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g->surface != EGL_NO_SURFACE) eglDestroySurface(g->display, g->surface);
        if (g->context != EGL_NO_CONTEXT) eglDestroyContext(g->display, g->context);
        eglTerminate(g->display);
    }
    g->display = EGL_NO_DISPLAY; g->context = EGL_NO_CONTEXT; g->surface = EGL_NO_SURFACE;
    g->program = g->texture = g->vao = 0; g->width = g->height = 0;
    g->full_upload = true;
}

static GLuint shader(EmberGL *g, GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, sizeof(g->error), NULL, g->error);
        glDeleteShader(s); return 0;
    }
    return s;
}

bool ember_gl_open(EmberGL *g, EGLNativeWindowType window, bool pbuffer, int w, int h)
{
    memset(g, 0, sizeof(*g));
    g->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g->display == EGL_NO_DISPLAY || !eglInitialize(g->display, NULL, NULL))
        return fail(g, "eglInitialize", eglGetError());
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail(g, "eglBindAPI", eglGetError());
    const EGLint attrs[] = {EGL_SURFACE_TYPE, pbuffer ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLConfig config; EGLint count = 0;
    if (!eglChooseConfig(g->display, attrs, &config, 1, &count) || !count)
        return fail(g, "ES3 config", eglGetError());
#ifdef __ANDROID__
    if (!pbuffer) {
        EGLint visual;
        if (!eglGetConfigAttrib(g->display, config, EGL_NATIVE_VISUAL_ID, &visual) ||
            ANativeWindow_setBuffersGeometry(window, w, h, visual))
            return fail(g, "EGL native visual", eglGetError());
    }
#endif
    const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g->context = eglCreateContext(g->display, config, EGL_NO_CONTEXT, ctx);
    if (g->context == EGL_NO_CONTEXT) return fail(g, "eglCreateContext", eglGetError());
    const EGLint pb[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    g->surface = pbuffer ? eglCreatePbufferSurface(g->display, config, pb) :
        eglCreateWindowSurface(g->display, config, window, NULL);
    if (g->surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(g->display, g->surface, g->surface, g->context))
        return fail(g, "EGL surface/current", eglGetError());
    /* AChoreographer paces submission; do not add a second vsync wait here. */
    eglSwapInterval(g->display, 0);
    snprintf(g->vendor, sizeof(g->vendor), "%s", glGetString(GL_VENDOR));
    snprintf(g->renderer, sizeof(g->renderer), "%s", glGetString(GL_RENDERER));
    snprintf(g->version, sizeof(g->version), "%s", glGetString(GL_VERSION));
    const char *vs = "#version 300 es\n"
        "out vec2 uv; void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
        "uv=vec2(p.x,1.0-p.y);gl_Position=vec4(p*2.0-1.0,0,1);}";
    const char *fs = "#version 300 es\nprecision highp float;"
        "uniform sampler2D image;in vec2 uv;out vec4 color;"
        "void main(){color=vec4(texture(image,uv).bgr,1.0);}";
    GLuint v = shader(g, GL_VERTEX_SHADER, vs), f = shader(g, GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { glDeleteShader(v); glDeleteShader(f); return false; }
    g->program = glCreateProgram(); glAttachShader(g->program, v); glAttachShader(g->program, f);
    glLinkProgram(g->program); glDeleteShader(v); glDeleteShader(f);
    GLint ok; glGetProgramiv(g->program, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(g->program, sizeof(g->error), NULL, g->error); return false; }
    glGenVertexArrays(1, &g->vao); glBindVertexArray(g->vao);
    glGenTextures(1, &g->texture); glBindTexture(GL_TEXTURE_2D, g->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_DEPTH_TEST);
    g->full_upload = true;
    return glGetError() == GL_NO_ERROR || fail(g, "GL setup", 1);
}

bool ember_gl_draw(EmberGL *g, const uint8_t *bgra, size_t size,
                   uint32_t w, uint32_t h, uint32_t stride,
                   uint32_t x, uint32_t y, uint32_t dw, uint32_t dh, uint64_t *uploaded)
{
    if (!bgra || !w || !h || w > 16384 || h > 16384 || stride < w * 4 || stride % 4 ||
        (uint64_t)stride * h > size || x > w || y > h || dw > w-x || dh > h-y)
        return fail(g, "invalid frame geometry", 0);
    glBindTexture(GL_TEXTURE_2D, g->texture);
    if (g->width != w || g->height != h) {
        GLint limit; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
        if (w > (uint32_t)limit || h > (uint32_t)limit) return fail(g, "texture limit", limit);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g->width = w; g->height = h; g->full_upload = true;
    }
    if (g->full_upload) { x=0; y=0; dw=w; dh=h; }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride/4);
    if (dw && dh) glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE,
                                 bgra + (size_t)y*stride + x*4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    EGLint vw, vh;
    if (!eglQuerySurface(g->display, g->surface, EGL_WIDTH, &vw) ||
        !eglQuerySurface(g->display, g->surface, EGL_HEIGHT, &vh))
        return fail(g, "eglQuerySurface", eglGetError());
    glViewport(0, 0, vw, vh); glUseProgram(g->program); glBindVertexArray(g->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLenum error = glGetError();
    if (error) return fail(g, "GL draw/upload", error);
    *uploaded += (uint64_t)dw*dh*4;
    g->full_upload = false;
    return true;
}

bool ember_gl_swap(EmberGL *g)
{
    return eglSwapBuffers(g->display, g->surface) || fail(g, "eglSwapBuffers", eglGetError());
}
