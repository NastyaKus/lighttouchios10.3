/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "presentation.h"
#include <stdio.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif
static void publish(EgPresenter *p) { if(p->options.publish)p->options.publish(p->options.opaque,&p->stats); }
static int fail(EgPresenter *p,const char *stage,unsigned code) {
    snprintf(p->error,sizeof(p->error),"%s: 0x%x",stage,code);return 2;
}
static int restore(EmberGL *owner) {
    return eglMakeCurrent(owner->display,owner->surface,owner->surface,owner->context);
}
static void release_window(EgPresenter *p,EGLNativeWindowType window) {
    if(window && p->options.release_window)p->options.release_window(p->options.opaque,window);
}
void eg_present_close(EgPresenter *p,EmberGL *owner) {
    if(p->context && p->surface && eglMakeCurrent(owner->display,p->surface,p->surface,p->context)) {
        glDeleteFramebuffers(1,&p->read_fbo);glDeleteFramebuffers(1,&p->mirror_fbo);glDeleteTextures(1,&p->mirror_texture);
    }
    restore(owner);
    if(p->surface)eglDestroySurface(owner->display,p->surface);
    if(p->context)eglDestroyContext(owner->display,p->context);
    release_window(p,p->window);
    p->window=(EGLNativeWindowType)0;p->surface=EGL_NO_SURFACE;p->context=EGL_NO_CONTEXT;
    p->read_fbo=p->mirror_fbo=p->mirror_texture=0;p->width=p->height=0;
}
static int create(EgPresenter *p,EmberGL *owner,EGLNativeWindowType window,uint64_t generation,uint32_t w,uint32_t h) {
    p->window=window;p->active_generation=generation;
    const EGLint attrs[]={EGL_SURFACE_TYPE,window?EGL_WINDOW_BIT:EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,
        EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,0,EGL_STENCIL_SIZE,0,EGL_NONE};
    EGLConfig config;EGLint count=0;
    if(!eglChooseConfig(owner->display,attrs,&config,1,&count) || !count)return fail(p,"present config",eglGetError());
    const EGLint context[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    p->context=eglCreateContext(owner->display,config,owner->context,context);
    if(!p->context)return fail(p,"present shared context",eglGetError());
#ifdef __ANDROID__
    if(window) {
        EGLint visual;
        if(!eglGetConfigAttrib(owner->display,config,EGL_NATIVE_VISUAL_ID,&visual) ||
           ANativeWindow_setBuffersGeometry(window,0,0,visual))return fail(p,"present window geometry",eglGetError());
    }
#endif
    const EGLint pb[]={EGL_WIDTH,(EGLint)w,EGL_HEIGHT,(EGLint)h,EGL_NONE};
    p->surface=window?eglCreateWindowSurface(owner->display,config,window,NULL):eglCreatePbufferSurface(owner->display,config,pb);
    if(!p->surface || !eglMakeCurrent(owner->display,p->surface,p->surface,p->context))return fail(p,"present surface/current",eglGetError());
    eglSwapInterval(owner->display,1);
    glGenFramebuffers(1,&p->read_fbo);glGenFramebuffers(1,&p->mirror_fbo);glGenTextures(1,&p->mirror_texture);
    glBindTexture(GL_TEXTURE_2D,p->mirror_texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glBindFramebuffer(GL_FRAMEBUFFER,p->mirror_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,p->mirror_texture,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return fail(p,"present mirror FBO",glGetError());
    glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_DITHER);glDisable(GL_DEPTH_TEST);
    p->width=w;p->height=h;p->stats.surface_creates++;
    return 0;
}
int eg_present(EgPresenter *p,EmberGL *owner,GLuint renderbuffer,uint32_t w,uint32_t h,uint32_t serial) {
    if(!renderbuffer || !w || !h || w>512 || h>512 || !serial)return fail(p,"present arguments",0);
    uint64_t generation=0;
    EGLNativeWindowType window=p->options.acquire_window?p->options.acquire_window(p->options.opaque,&generation):(EGLNativeWindowType)0;
    if(!window && p->options.require_window) {
        if(p->surface){p->stats.surface_losses++;eg_present_close(p,owner);}
        publish(p);return 1;
    }
    if(!p->context || p->active_generation!=generation || p->window!=window || p->width!=w || p->height!=h) {
        eg_present_close(p,owner);
        int status=create(p,owner,window,generation,w,h);
        if(status){restore(owner);eg_present_close(p,owner);publish(p);return status;}
    } else release_window(p,window);
    /* Publish producer writes before consuming the shared renderbuffer. */
    if(!restore(owner))return fail(p,"restore producer",eglGetError());
    GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);glFlush();
    if(!fence)return fail(p,"producer fence",glGetError());
    int status=0;
    if(!eglMakeCurrent(owner->display,p->surface,p->surface,p->context))status=fail(p,"present current",eglGetError());
    else {
        glWaitSync(fence,0,GL_TIMEOUT_IGNORED);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,p->read_fbo);
        glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,renderbuffer);
        if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)status=fail(p,"present source FBO",glGetError());
        else {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,p->mirror_fbo);
            glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,p->mirror_fbo);
            /* Four diagnostic pixels only; the displayed image stays on GPU. */
            uint8_t samples[16];
            const GLint x[]={w/8,7*w/8,w/8,7*w/8},y[]={h/8,h/8,7*h/8,7*h/8};
            glPixelStorei(GL_PACK_ALIGNMENT,1);
            for(int i=0;i<4;i++)glReadPixels(x[i],y[i],1,1,GL_RGBA,GL_UNSIGNED_BYTE,samples+i*4);
            EGLint target_w=0,target_h=0;
            if(!eglQuerySurface(owner->display,p->surface,EGL_WIDTH,&target_w) ||
               !eglQuerySurface(owner->display,p->surface,EGL_HEIGHT,&target_h) || target_w<=0 || target_h<=0)
                status=fail(p,"present dimensions",eglGetError());
            else {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
                glBlitFramebuffer(0,0,w,h,0,0,target_w,target_h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
                GLenum error=glGetError();
                if(error)status=fail(p,"present blit",error);
                else if(!eglSwapBuffers(owner->display,p->surface))status=fail(p,"present swap",eglGetError());
                else {
                    p->stats.frames++;if(p->window)p->stats.window_frames++;
                    p->stats.last_serial=serial;p->stats.generation=generation;p->stats.width=w;p->stats.height=h;
                    p->stats.sample_pixels+=4;
                    for(int i=0;i<4;i++)p->stats.pixels[i]=(uint32_t)samples[i*4]|(uint32_t)samples[i*4+1]<<8|
                        (uint32_t)samples[i*4+2]<<16|(uint32_t)samples[i*4+3]<<24;
                }
            }
        }
    }
    if(!restore(owner))status=fail(p,"restore producer after present",eglGetError());
    glDeleteSync(fence);publish(p);return status;
}
