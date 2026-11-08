/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "present-probe.h"
#ifdef EGPU_GUEST_FREESTANDING
#undef NULL
#include "../guest/darwin-abi.h"
#else
#include <stdio.h>
#include <unistd.h>
#endif
static int allocate(void *opaque,uint32_t w,uint32_t h) {
    (void)opaque;glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8_OES,w,h);return glGetError()==GL_NO_ERROR;
}
static int submit(void *opaque) { (void)opaque;return egGlesPresent(); }
static GLuint shader(GLenum kind,const char *text) {
    GLuint s=glCreateShader(kind);glShaderSource(s,1,&text,NULL);glCompileShader(s);GLint ok=0;
    glGetShaderiv(s,GL_COMPILE_STATUS,&ok);return ok?s:0;
}
int egProbePresentation(EgGlesClient *c,void *opaque,int (*storage)(void *,uint32_t,uint32_t),int (*present)(void *)) {
    if(!storage)storage=allocate;
    if(!present)present=submit;
    unsigned checks=0;GLuint rbo=0,fbo=0,empty=0,buffer=0,vs=0,fs=0,program=0;
#define CHECK(x) do { checks++;if(!(x)){dprintf(1,"*** EGPU-PRESENT: FAIL line=%u condition=%s error=0x%x\n",__LINE__,#x,glGetError());goto fail;} } while(0)
    glGenRenderbuffers(1,&rbo);glBindRenderbuffer(GL_RENDERBUFFER,rbo);CHECK(rbo && storage(opaque,64,64));
    GLint value=0;glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_WIDTH,&value);CHECK(value==64);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_HEIGHT,&value);CHECK(value==64);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_INTERNAL_FORMAT,&value);CHECK(value==GL_RGBA8_OES);
    glGenFramebuffers(1,&fbo);glGenFramebuffers(1,&empty);glBindFramebuffer(GL_FRAMEBUFFER,empty);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rbo);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&value);CHECK((GLuint)value==fbo);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8_OES,513,64);CHECK(glGetError()==GL_INVALID_VALUE);
    vs=shader(GL_VERTEX_SHADER,"attribute vec2 position;void main(){gl_Position=vec4(position,0.,1.);}");
    fs=shader(GL_FRAGMENT_SHADER,"precision mediump float;void main(){gl_FragColor=vec4(1.,.65,0.,1.);}");CHECK(vs && fs);
    program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glBindAttribLocation(program,0,"position");glLinkProgram(program);
    glGetProgramiv(program,GL_LINK_STATUS,&value);CHECK(value==GL_TRUE);glUseProgram(program);
    glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    glViewport(0,0,64,64);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);
    uint8_t challenge[3];egGlesChallenge(c,challenge);
    uint32_t top_left=(uint32_t)challenge[0]|(uint32_t)challenge[1]<<8|(uint32_t)challenge[2]<<16|0xff000000u;
    dprintf(1,"*** EGPU-PRESENT: start frames=60 challenge=%02x%02x%02x\n",challenge[0],challenge[1],challenge[2]);
    for(int frame=0;frame<60;frame++) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(0,0,32,32);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
        glScissor(32,0,32,32);glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);
        glScissor(0,32,32,32);glClearColor(challenge[0]/255.f,challenge[1]/255.f,challenge[2]/255.f,1);glClear(GL_COLOR_BUFFER_BIT);
        glScissor(32,32,32,32);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glDisable(GL_SCISSOR_TEST);
        float x=(frame%20-10)/60.f;GLfloat vertices[]={x-.16f,-.2f,x+.16f,-.2f,x,.22f};
        glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_DYNAMIC_DRAW);glDrawArrays(GL_TRIANGLES,0,3);
        int ok=0;for(int retry=0;retry<100 && !ok;retry++){ok=present(opaque);if(!ok)usleep(100000);}
        CHECK(ok);
        uint32_t samples[4];egGlesPresentSamples(c,samples);
        CHECK(samples[0]==0xffff0000u && samples[1]==0xffffffffu && samples[2]==top_left && samples[3]==0xff00ff00u);
        CHECK(egGlesPresentedFrames(c)==(uint32_t)frame+1 && glGetError()==GL_NO_ERROR);
        usleep(100000);
    }
    glDisableVertexAttribArray(0);glUseProgram(0);glBindFramebuffer(GL_FRAMEBUFFER,0);glBindRenderbuffer(GL_RENDERBUFFER,0);
    glDeleteFramebuffers(1,&fbo);glDeleteFramebuffers(1,&empty);glDeleteRenderbuffers(1,&rbo);glDeleteBuffers(1,&buffer);
    glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);CHECK(glGetError()==GL_NO_ERROR);
    dprintf(1,"*** EGPU-PRESENT: PASS frames=%u window_frames=%u checks=%u shared-renderbuffer GPU blit; four sample pixels per frame\n",egGlesPresentedFrames(c),egGlesWindowFrames(c),checks);return 1;
fail:return 0;
#undef CHECK
}
#ifdef EGPU_PRESENT_LINUX_PROBE
int egProbeExtraChecks(EgGlesClient *c) { return egProbePresentation(c,NULL,NULL,NULL); }
#endif
