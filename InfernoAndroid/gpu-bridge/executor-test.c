/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static EgpuExecutor executor;
static EgpuSession session;
static uint8_t request[EGPU_PAGE],reply[EGPU_PAGE];
static uint32_t seq;
static EgpuFrame response;
static int send(uint16_t op,uint8_t *p,uint32_t n) {
    assert(!egpu_encode(request,op,++seq,p,n));
    assert(!egpu_session_process(&executor,&session,request,reply));
    assert(!egpu_decode(reply,EGPU_PAGE,&response));
    assert(response.seq==seq && response.opcode==(op|EGPU_RESPONSE));return egpu_u32(response.payload);
}
int main(void) {
    uint8_t p[64]={0},old[EGPU_PAGE];
    assert(!egpu_executor_open(&executor));printf("GL: %s / %s\n",executor.context.vendor,executor.context.renderer);
    assert(!egpu_encode(request,EGPU_CREATE,1,p,8));
    assert(!egpu_session_process(&executor,&session,request,reply));
    assert(!egpu_decode(reply,EGPU_PAGE,&response));assert(egpu_u32(response.payload)==EGPU_SEQUENCE);
    assert(send(EGPU_HELLO,p,0)==EGPU_OK);
    assert(send(EGPU_CREATE,p,8)==EGPU_BAD_ARGUMENT);
    egpu_put32(p,513);egpu_put32(p+4,64);assert(send(EGPU_CREATE,p,8)==EGPU_BAD_ARGUMENT);
    egpu_put32(p,64);assert(send(EGPU_CREATE,p,8)==EGPU_OK);
    uint32_t handle=egpu_u32(response.payload+4);memcpy(old,reply,EGPU_PAGE);
    uint64_t commands=executor.commands;
    request[EGPU_PAGE-1]=99; /* Equivalent frame with different transport padding. */
    assert(!egpu_session_process(&executor,&session,request,reply));
    assert(executor.commands==commands && session.replayed==1 && !memcmp(old,reply,EGPU_PAGE));
    egpu_put32(p,handle);p[4]=0;p[5]=0;p[6]=255;p[7]=255;
    assert(send(EGPU_CLEAR,p,8)==EGPU_OK);
    const float vertices[]={-.75f,-.75f,.75f,-.75f,0,.75f};
    for(int i=0;i<6;i++){uint32_t bits;memcpy(&bits,vertices+i,4);egpu_put32(p+4+i*4,bits);}
    p[28]=255;p[29]=0;p[30]=0;p[31]=255;
    assert(send(EGPU_TRIANGLE,p,32)==EGPU_OK);
    egpu_put32(p+4,0x7fc00000u);assert(send(EGPU_TRIANGLE,p,32)==EGPU_BAD_ARGUMENT);
    assert(send(EGPU_FENCE,p,4)==EGPU_OK);
    egpu_put32(p+4,2);egpu_put32(p+8,32);egpu_put32(p+12,32);egpu_put32(p+16,0);egpu_put32(p+20,0);
    assert(send(EGPU_READ_POINTS,p,24)==EGPU_OK);
    const uint8_t pixels[]={255,0,0,255,0,0,255,255};assert(response.size==16 && !memcmp(response.payload+8,pixels,8));
    egpu_put32(p+8,64);assert(send(EGPU_READ_POINTS,p,24)==EGPU_BAD_ARGUMENT);
    egpu_put32(p+4,0xffffffffu);assert(send(EGPU_READ_POINTS,p,8)==EGPU_BAD_ARGUMENT);
    assert(send(0x1234,p,0)==EGPU_BAD_COMMAND);
    assert(send(EGPU_DESTROY,p,4)==EGPU_OK);
    assert(send(EGPU_DESTROY,p,4)==EGPU_BAD_HANDLE);
    egpu_put32(p,2);egpu_put32(p+4,2);assert(send(EGPU_CREATE,p,8)==EGPU_OK);
    uint32_t second=egpu_u32(response.payload+4);assert(second!=handle);
    egpu_put32(p,handle);assert(send(EGPU_CLEAR,p,8)==EGPU_BAD_HANDLE);
    egpu_put32(p,second);assert(send(EGPU_DESTROY,p,4)==EGPU_OK);
    egpu_executor_close(&executor);
    puts("PASS executor: real GL readback, bounded surfaces, stale handles, NaN, fence, errors, exactly-once replay");return 0;
}
