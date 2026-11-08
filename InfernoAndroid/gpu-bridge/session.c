/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session.h"
#include <string.h>
int egpu_session_process(EgpuExecutor *e,EgpuSession *s,const uint8_t request[EGPU_PAGE],uint8_t reply[EGPU_PAGE]) {
    EgpuFrame r;
    if(egpu_decode(request,EGPU_PAGE,&r) || (r.opcode&EGPU_RESPONSE))return EGPU_BAD_FRAME;
    if(r.seq==s->last_seq && !memcmp(request,s->request,EGPU_HEADER+r.size)) {
        memcpy(reply,s->reply,EGPU_PAGE);s->replayed++;return EGPU_OK;
    }
    if(s->last_seq==UINT32_MAX || r.seq!=s->last_seq+1 || (!s->last_seq && r.opcode!=EGPU_HELLO)) {
        uint8_t status[4];egpu_put32(status,EGPU_SEQUENCE);
        return egpu_encode(reply,r.opcode|EGPU_RESPONSE,r.seq,status,4);
    }
    int result=egpu_execute(e,&r,reply);
    if(!result) { s->last_seq=r.seq;memcpy(s->request,request,EGPU_PAGE);memcpy(s->reply,reply,EGPU_PAGE); }
    return result;
}
