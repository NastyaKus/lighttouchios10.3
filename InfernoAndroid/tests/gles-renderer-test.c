/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gles_renderer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned assertions;
static int fail_swap;
EGLBoolean __real_eglSwapBuffers(EGLDisplay display, EGLSurface surface);
EGLBoolean __wrap_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    if (fail_swap) return EGL_FALSE;
    return __real_eglSwapBuffers(display,surface);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s (GL: %s)\n",__LINE__,#x,g.error); return 1; } assertions++; } while(0)
static void pattern(unsigned char *src, unsigned w, unsigned h, unsigned stride) {
    memset(src,0xee,stride*h);
    for (unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
        unsigned char *p=src+y*stride+x*4;
        p[0]=10+y; p[1]=30+x; p[2]=90+x+y; p[3]=x; /* BGRA, nonopaque input */
    }
}
static int verify(unsigned char *src, unsigned w, unsigned h, unsigned stride) {
    unsigned char rgba[4*8*8];
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    if(glGetError()!=GL_NO_ERROR) return 0;
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
        unsigned char *p=src+y*stride+x*4, *q=rgba+((h-1-y)*w+x)*4;
        if(q[0]!=p[2] || q[1]!=p[1] || q[2]!=p[0] || q[3]!=255) return 0;
    }
    return 1;
}
int main(void) {
    EmberGL g={0}; uint64_t bytes=0;
    unsigned char frame[256];
    CHECK(ember_gl_open(&g, (EGLNativeWindowType)0, true, 3, 2));
    printf("GL: %s / %s / %s\n",g.vendor,g.renderer,g.version);
    pattern(frame,3,2,20);
    CHECK(ember_gl_draw(&g,frame,40,3,2,20,0,0,3,2,&bytes));
    CHECK(bytes==24); CHECK(verify(frame,3,2,20));
    frame[20+4]=199; frame[20+6]=201;
    CHECK(ember_gl_draw(&g,frame,40,3,2,20,1,1,1,1,&bytes));
    CHECK(bytes==28); CHECK(verify(frame,3,2,20));
    /* Zero damage redraws from the retained texture without another upload. */
    CHECK(ember_gl_draw(&g,frame,40,3,2,20,0,0,0,0,&bytes));
    CHECK(bytes==28); CHECK(verify(frame,3,2,20));
    CHECK(!ember_gl_draw(&g,frame,39,3,2,20,0,0,3,2,&bytes));
    CHECK(!ember_gl_draw(&g,frame,40,3,2,20,2,0,2,1,&bytes));
    CHECK(!ember_gl_draw(&g,frame,40,3,2,11,0,0,3,2,&bytes));
    CHECK(ember_gl_swap(&g));
    fail_swap=1;
    CHECK(!ember_gl_swap(&g)); CHECK(strstr(g.error,"eglSwapBuffers") != NULL);
    fail_swap=0;
    CHECK(ember_gl_swap(&g));
    /* Texture resize on the same context, then context/surface recreation. */
    pattern(frame,2,2,12);
    CHECK(ember_gl_draw(&g,frame,24,2,2,12,0,0,2,2,&bytes));
    CHECK(g.width==2 && g.height==2 && bytes==44);
    ember_gl_close(&g); ember_gl_close(&g);
    CHECK(ember_gl_open(&g,(EGLNativeWindowType)0,true,4,3));
    pattern(frame,4,3,24);
    CHECK(ember_gl_draw(&g,frame,72,4,3,24,2,1,1,1,&bytes));
    CHECK(bytes==92); CHECK(verify(frame,4,3,24));
    ember_gl_close(&g);
    printf("PASS %u checks: BGRA, forced alpha, top-down orientation, padded stride, dirty rect, retained texture, bounds, resize, recreate.\n",assertions);
    return 0;
}
