/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static EgpuExecutor executor;
static EgpuSession session;
static uint32_t sequence;
static uint8_t request[EGPU_PAGE],reply[EGPU_PAGE];
static EgpuFrame response;
static int send(uint16_t opcode,const uint8_t *p,uint32_t n) {
    assert(!egpu_encode(request,opcode,++sequence,p,n));
    assert(!egpu_session_process(&executor,&session,request,reply));
    assert(!egpu_decode(reply,EGPU_PAGE,&response));return egpu_u32(response.payload);
}
static uint32_t append(uint8_t *p,uint32_t at,uint16_t op,const uint32_t *values,uint32_t count) {
    egpu_put16(p+at,op);egpu_put16(p+at+2,count*4);
    for(uint32_t i=0;i<count;i++)egpu_put32(p+at+4+i*4,values[i]);
    return at+4+count*4;
}
static uint32_t get_error(void) {
    uint8_t p[12]={0};egpu_put32(p,EG_ERROR_QUERY);
    assert(send(EGPU_GLES_QUERY,p,12)==EGPU_OK && response.size==8);return egpu_u32(response.payload+4);
}
int main(void) {
    assert(!egpu_executor_open(&executor));uint8_t p[128]={0};
    assert(send(EGPU_HELLO,p,0)==EGPU_OK);
    egpu_put32(p,8);egpu_put32(p+4,8);assert(send(EGPU_CREATE,p,8)==EGPU_OK);
    uint64_t calls=executor.gles.calls;
    uint32_t make[]={EG_PROGRAM,1,0};egpu_put32(p,2);uint32_t at=append(p,4,EG_NEW,make,3);
    egpu_put16(p+at,0xffff);egpu_put16(p+at+2,0);
    assert(send(EGPU_GLES_BATCH,p,at+4)==EGPU_BAD_COMMAND);
    assert(executor.gles.calls==calls && executor.gles.objects[1].kind==0 && !executor.gles.vao);
    egpu_put32(p,1);at=append(p,4,EG_NEW,make,3);
    assert(send(EGPU_GLES_BATCH,p,at-1)==EGPU_BAD_ARGUMENT);
    assert(!executor.gles.objects[1].kind);
    assert(send(EGPU_GLES_BATCH,p,at)==EGPU_OK && executor.gles.objects[1].kind==EG_PROGRAM);
    egpu_put32(p,EG_PROGRAM_IV);egpu_put32(p+4,1);egpu_put32(p+8,0x8267);
    assert(send(EGPU_GLES_QUERY,p,12)==EGPU_OK && response.size==8 && !egpu_u32(response.payload+4));
    assert(get_error()==GL_INVALID_ENUM && get_error()==GL_NO_ERROR);
    uint32_t bad[]={EG_BUFFER,UINT32_MAX,0};egpu_put32(p,1);at=append(p,4,EG_NEW,bad,3);
    assert(send(EGPU_GLES_BATCH,p,at)==EGPU_OK && get_error()==GL_INVALID_VALUE);
    uint32_t enable[]={0x8c89,1}; /* ES3 RASTERIZER_DISCARD is outside this ES2 subset. */
    egpu_put32(p,1);at=append(p,4,EG_ENABLE,enable,2);
    assert(send(EGPU_GLES_BATCH,p,at)==EGPU_OK && get_error()==GL_INVALID_ENUM);
    egpu_put32(p,EG_READ_RECT);egpu_put32(p+4,0);egpu_put32(p+8,0);egpu_put32(p+12,9);egpu_put32(p+16,1);
    assert(send(EGPU_GLES_QUERY,p,20)==EGPU_BAD_ARGUMENT && executor.pixels_read==0);
    uint32_t rbo[]={EG_RENDERBUFFER,2,0},bind_rbo[]={GL_RENDERBUFFER,2},storage[]={GL_RENDERBUFFER,GL_RGBA8,16,16};
    uint32_t fbo[]={EG_FRAMEBUFFER,3,0},bind_fbo[]={GL_FRAMEBUFFER,3},attach[]={GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,2};
    egpu_put32(p,6);at=append(p,4,EG_NEW,rbo,3);at=append(p,at,EG_BIND_RBO,bind_rbo,2);
    at=append(p,at,EG_RBO_STORAGE,storage,4);at=append(p,at,EG_NEW,fbo,3);
    at=append(p,at,EG_BIND_FBO,bind_fbo,2);at=append(p,at,EG_FBO_ATTACH,attach,4);
    assert(send(EGPU_GLES_BATCH,p,at)==EGPU_OK && get_error()==GL_NO_ERROR);
    glDisable(GL_SCISSOR_TEST);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);glScissor(0,0,8,16);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glViewport(3,4,5,6);
    EGLContext owner=eglGetCurrentContext();egpu_put32(p,2);egpu_put32(p+4,1);
    assert(send(EGPU_GLES_PRESENT,p,8)==EGPU_OK && response.size==48 && !egpu_u32(response.payload+4));
    assert(executor.presenter.stats.frames==1 && executor.presenter.stats.window_frames==0);
    assert(executor.presenter.stats.pixels[0]==0xff0000ffu && executor.presenter.stats.pixels[1]==0xffff0000u);
    assert(eglGetCurrentContext()==owner && glIsEnabled(GL_SCISSOR_TEST));
    GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);assert(viewport[0]==3 && viewport[1]==4 && viewport[2]==5 && viewport[3]==6);
    uint8_t previous[EGPU_PAGE];memcpy(previous,reply,EGPU_PAGE);
    assert(!egpu_session_process(&executor,&session,request,reply) && !memcmp(previous,reply,EGPU_PAGE));
    assert(executor.presenter.stats.frames==1); /* Replay must not swap a second frame. */
    egpu_put32(p,2);egpu_put32(p+4,3);assert(send(EGPU_GLES_PRESENT,p,8)==EGPU_SEQUENCE);
    executor.presenter.options.require_window=1;egpu_put32(p+4,2);
    assert(send(EGPU_GLES_PRESENT,p,8)==EGPU_OK && egpu_u32(response.payload+4)==1 && executor.presenter.stats.frames==1);
    executor.presenter.options.require_window=0;egpu_put32(p+4,3);
    assert(send(EGPU_GLES_PRESENT,p,8)==EGPU_OK && !egpu_u32(response.payload+4) && executor.presenter.stats.frames==2);
    egpu_executor_close(&executor);
    puts("PASS GLES wire: atomic structural validation, scalar-query whitelist, resource bounds, unsupported caps, readback bounds");
    puts("PASS presentation: shared RBO pixels, preserved producer state, exactly-once present replay, missing-window refusal and headless recreation");
    return 0;
}
