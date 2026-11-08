/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "surface.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif
static int fail(EgsSurfacePool *p,const char *message) { snprintf(p->error,sizeof(p->error),"%s",message);return 0; }
static int current(EgsSurfacePool *p) { return p->owner && eglGetCurrentContext()==p->owner; }
static EgsSurface *find(EgsSurfacePool *p,uint64_t context,uint64_t id) {
    if(!context || !id || !current(p))return NULL;
    for(unsigned i=0;i<EGS_MAX_SURFACES;i++)if(p->surfaces[i].id==id && p->surfaces[i].context==context)return p->surfaces+i;
    return NULL;
}
const EgsSurface *egs_surface_get(EgsSurfacePool *p,uint64_t context,uint64_t id) { return find(p,context,id); }
int egs_pool_open(EgsSurfacePool *p,uint64_t budget) {
    memset(p,0,sizeof(*p));GLint version=0,maximum=0;
    if(!eglGetCurrentContext())return fail(p,"No transfer EGL context");
    glGetIntegerv(GL_MAJOR_VERSION,&version);glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
    if(version<3 || maximum<2048 || glGetError()!=GL_NO_ERROR || !budget || budget>EGS_MAX_BYTES)return fail(p,"ES3/texture budget unavailable");
    p->owner=eglGetCurrentContext();p->maximum=maximum>8192?8192:(uint32_t)maximum;p->budget=budget;return 1;
}
void egs_pool_close(EgsSurfacePool *p) {
    if(p->owner && !current(p)){fail(p,"Pool closed outside its transfer context");return;}
    if(current(p))for(unsigned i=0;i<EGS_MAX_SURFACES;i++)if(p->surfaces[i].id) {
        glDeleteFramebuffers(1,&p->surfaces[i].framebuffer);glDeleteTextures(1,&p->surfaces[i].texture);
    }
    memset(p,0,sizeof(*p));
}
int egs_surface_create(EgsSurfacePool *p,uint64_t context,uint64_t id,uint32_t w,uint32_t h,uint32_t format) {
    uint64_t bytes=(uint64_t)w*h*4;EgsSurface *slot=NULL;
    if(!current(p) || !context || !id || !w || !h || w>p->maximum || h>p->maximum ||
       (format!=EGS_RGBA && format!=EGS_BGRA) || bytes>64u*1024u*1024u || bytes>p->budget-p->bytes)return fail(p,"Invalid surface/budget");
    for(unsigned i=0;i<EGS_MAX_SURFACES;i++) {
        if(p->surfaces[i].id==id)return fail(p,"Duplicate surface identifier");
        if(!p->surfaces[i].id && !slot)slot=p->surfaces+i;
    }
    if(!slot)return fail(p,"Surface table full");
    EgsSurface surface={.id=id,.context=context,.bytes=bytes,.width=w,.height=h,.format=format};
    GLint texture,draw,read,unpack;GLboolean mask[4],scissor=glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glGenTextures(1,&surface.texture);glBindTexture(GL_TEXTURE_2D,surface.texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)w,(GLsizei)h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&surface.framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,surface.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,surface.texture,0);
    int ok=glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if(ok){const GLfloat zero[4]={0,0,0,0};glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearBufferfv(GL_COLOR,0,zero);}
    glColorMask(mask[0],mask[1],mask[2],mask[3]);
    if(scissor)glEnable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D,(GLuint)texture);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)read);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,(GLuint)unpack);ok=ok && glGetError()==GL_NO_ERROR;
    if(!ok){glDeleteFramebuffers(1,&surface.framebuffer);glDeleteTextures(1,&surface.texture);return fail(p,"Cannot allocate surface");}
    *slot=surface;p->bytes+=bytes;return 1;
}
int egs_surface_destroy(EgsSurfacePool *p,uint64_t context,uint64_t id) {
    EgsSurface *s=find(p,context,id);if(!s)return fail(p,"Unknown/cross-context surface");
    // The budget is returned only after prior writers and presenters have
    // completed. Their fences are queued into this transfer context.
    glFinish();
    glDeleteFramebuffers(1,&s->framebuffer);glDeleteTextures(1,&s->texture);p->bytes-=s->bytes;memset(s,0,sizeof(*s));return 1;
}
static int rect(EgsSurface *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
    return w && h && x<=s->width && y<=s->height && w<=s->width-x && h<=s->height-y;
}
int egs_surface_upload(EgsSurfacePool *p,uint64_t context,uint64_t id,uint64_t previous,uint64_t revision,
    uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t row_bytes,const uint8_t *data,uint32_t size) {
    EgsSurface *s=find(p,context,id);
    if(!s || previous!=s->revision || revision<=previous || !data || !rect(s,x,y,w,h) || row_bytes<(uint64_t)w*4 || row_bytes%4 ||
       row_bytes>64u*1024u*1024u || (uint64_t)(h-1)*row_bytes+(uint64_t)w*4!=size || size>64u*1024u*1024u)return fail(p,"Invalid dirty surface upload");
    uint8_t *converted=NULL;
    if(s->format==EGS_BGRA) {
        size_t tight=(size_t)w*h*4;converted=malloc(tight);if(!converted)return fail(p,"Dirty rectangle allocation");
        for(uint32_t row=0;row<h;row++)for(uint32_t col=0;col<w;col++) {
            const uint8_t *src=data+(size_t)row*row_bytes+col*4;uint8_t *dst=converted+((size_t)row*w+col)*4;
            dst[0]=src[2];dst[1]=src[1];dst[2]=src[0];dst[3]=src[3];
        }
        data=converted;
    }
    GLint texture,alignment,length,rows,pixels,buffer;
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);glGetIntegerv(GL_UNPACK_ROW_LENGTH,&length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS,&rows);glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&pixels);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glBindTexture(GL_TEXTURE_2D,s->texture);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,converted?0:(GLint)(row_bytes/4));glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    glTexSubImage2D(GL_TEXTURE_2D,0,(GLint)x,(GLint)y,(GLsizei)w,(GLsizei)h,GL_RGBA,GL_UNSIGNED_BYTE,data);
    glBindTexture(GL_TEXTURE_2D,(GLuint)texture);glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);glPixelStorei(GL_UNPACK_ROW_LENGTH,length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,rows);glPixelStorei(GL_UNPACK_SKIP_PIXELS,pixels);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,(GLuint)buffer);
    free(converted);if(glGetError()!=GL_NO_ERROR)return fail(p,"Dirty texture upload failed");
    s->revision=revision;p->cpu_uploaded+=(uint64_t)w*h*4;return 1;
}
int egs_surface_gpu_written(EgsSurfacePool *p,uint64_t context,uint64_t id,uint64_t previous,uint64_t revision) {
    EgsSurface *s=find(p,context,id);if(!s || previous!=s->revision || revision<=previous)return fail(p,"GPU surface version conflict");
    s->revision=revision;return 1;
}
int egs_surface_read(EgsSurfacePool *p,uint64_t context,uint64_t id,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint8_t *data,uint32_t size) {
    EgsSurface *s=find(p,context,id);
    if(!s || !data || !rect(s,x,y,w,h) || (uint64_t)w*h*4!=size || size>64u*1024u*1024u)return fail(p,"Invalid explicit CPU read");
    GLint read,alignment,length,rows,pixels,buffer;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);glGetIntegerv(GL_PACK_ROW_LENGTH,&length);
    glGetIntegerv(GL_PACK_SKIP_ROWS,&rows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&pixels);glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,s->framebuffer);glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
    glReadPixels((GLint)x,(GLint)y,(GLsizei)w,(GLsizei)h,GL_RGBA,GL_UNSIGNED_BYTE,data);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)read);glBindBuffer(GL_PIXEL_PACK_BUFFER,(GLuint)buffer);glPixelStorei(GL_PACK_ALIGNMENT,alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH,length);glPixelStorei(GL_PACK_SKIP_ROWS,rows);glPixelStorei(GL_PACK_SKIP_PIXELS,pixels);
    if(glGetError()!=GL_NO_ERROR)return fail(p,"Explicit CPU read failed");
    if(s->format==EGS_BGRA)for(uint32_t i=0;i<size;i+=4){uint8_t temp=data[i];data[i]=data[i+2];data[i+2]=temp;}
    p->cpu_read+=size;return 1;
}
static void release(EgsPresenter *p,EGLNativeWindowType window) { if(window && p->options.release_window)p->options.release_window(p->options.opaque,window); }
static void publish(EgsPresenter *p) { if(p->options.publish)p->options.publish(p->options.opaque,&p->stats); }
static int restore(EmberGL *g) { return eglMakeCurrent(g->display,g->surface,g->surface,g->context); }
void egs_present_close(EgsPresenter *p,EmberGL *g) {
    if(p->context && p->surface && eglMakeCurrent(g->display,p->surface,p->surface,p->context)){glDeleteFramebuffers(1,&p->framebuffer);glFinish();}
    restore(g);if(p->surface)eglDestroySurface(g->display,p->surface);if(p->context)eglDestroyContext(g->display,p->context);
    release(p,p->window);p->context=EGL_NO_CONTEXT;p->surface=EGL_NO_SURFACE;p->window=(EGLNativeWindowType)0;p->framebuffer=0;
}
static int present_fail(EgsPresenter *p,const char *message) { snprintf(p->error,sizeof(p->error),"%s EGL=0x%x",message,eglGetError());return 2; }
int egs_present(EgsPresenter *p,EmberGL *g,EgsSurfacePool *pool,uint64_t context,uint64_t id,uint64_t serial) {
    const EgsSurface *s=egs_surface_get(pool,context,id);
    if(!s || !serial || serial<=p->stats.last_serial || pool->owner!=g->context)return present_fail(p,"Invalid presentation identity");
    uint64_t generation=0;
    EGLNativeWindowType window=p->options.acquire_window?p->options.acquire_window(p->options.opaque,&generation):(EGLNativeWindowType)0;
    if(!window && p->options.require_window) {
        if(p->surface){p->stats.surface_losses++;egs_present_close(p,g);}publish(p);return 1;
    }
    if(!p->context || p->window!=window || p->generation!=generation || (!window && (p->stats.width!=s->width || p->stats.height!=s->height))) {
        egs_present_close(p,g);p->window=window;p->generation=generation;
        const EGLint attrs[]={EGL_SURFACE_TYPE,window?EGL_WINDOW_BIT:EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,0,EGL_STENCIL_SIZE,0,EGL_NONE};
        EGLConfig config;EGLint count=0;
        if(!eglChooseConfig(g->display,attrs,&config,1,&count) || !count)goto failed;
        const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};p->context=eglCreateContext(g->display,config,g->context,ca);if(!p->context)goto failed;
#ifdef __ANDROID__
        if(window){EGLint visual;if(!eglGetConfigAttrib(g->display,config,EGL_NATIVE_VISUAL_ID,&visual) || ANativeWindow_setBuffersGeometry(window,0,0,visual))goto failed;}
#endif
        const EGLint pa[]={EGL_WIDTH,(EGLint)s->width,EGL_HEIGHT,(EGLint)s->height,EGL_NONE};
        p->surface=window?eglCreateWindowSurface(g->display,config,window,NULL):eglCreatePbufferSurface(g->display,config,pa);
        if(!p->surface || !eglMakeCurrent(g->display,p->surface,p->surface,p->context))goto failed;
        eglSwapInterval(g->display,0);glGenFramebuffers(1,&p->framebuffer);
        glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_DITHER);glDisable(GL_DEPTH_TEST);p->stats.surface_creates++;
    } else release(p,window);
    if(!restore(g))goto failed;
    GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);glFlush();if(!fence)goto failed;
    GLsync consumed=NULL;
    int ok=eglMakeCurrent(g->display,p->surface,p->surface,p->context);
    if(ok) {
        glWaitSync(fence,0,GL_TIMEOUT_IGNORED);glBindFramebuffer(GL_READ_FRAMEBUFFER,p->framebuffer);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->texture,0);
        EGLint w=0,h=0;
        ok=glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE &&
            eglQuerySurface(g->display,p->surface,EGL_WIDTH,&w) && eglQuerySurface(g->display,p->surface,EGL_HEIGHT,&h) && w>0 && h>0;
        if(ok) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);glBlitFramebuffer(0,0,s->width,s->height,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
            // Do not keep a dead guest surface alive through the presenter FBO.
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
            ok=glGetError()==GL_NO_ERROR && eglSwapBuffers(g->display,p->surface);
            consumed=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);glFlush();if(!consumed)ok=0;
        }
    }
    glDeleteSync(fence);if(!restore(g))ok=0;
    if(consumed){glWaitSync(consumed,0,GL_TIMEOUT_IGNORED);glDeleteSync(consumed);if(glGetError()!=GL_NO_ERROR)ok=0;}
    if(!ok)goto failed;
    p->stats.frames++;if(p->window)p->stats.window_frames++;p->stats.last_serial=serial;
    p->stats.generation=generation;p->stats.width=s->width;p->stats.height=s->height;publish(p);return 0;
failed:
    present_fail(p,"System texture presentation failed");egs_present_close(p,g);publish(p);return 2;
}
