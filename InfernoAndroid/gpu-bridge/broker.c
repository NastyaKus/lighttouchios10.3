/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "broker.h"
static int authenticate(int client,EgMailbox *mailbox,const uint8_t token[16],eg_uid_t uid,eg_gid_t gid) {
    uint8_t hello[EG_BROKER_HELLO_BYTES],reply[EG_BROKER_REPLY_BYTES]={0};eg_uid_t peer_uid=0;eg_gid_t peer_gid=0;
    if(!eg_stream_peer(client,&peer_uid,&peer_gid) || !eg_stream_read(client,hello,sizeof(hello),eg_stream_now()+2000))return 0;
    unsigned diff=0;for(int i=0;i<16;i++)diff|=hello[8+i]^token[i];
    for(int i=24;i<32;i++)diff|=hello[i];
    int ok=peer_uid==uid && peer_gid==gid && !diff && !memcmp(hello,"EGB1",4) && egpu_u32(hello+4)==1;
    memcpy(reply,"EGB1",4);egpu_put32(reply+4,1);egpu_put32(reply+8,!ok);
    if(ok){egpu_put32(reply+12,64);memcpy(reply+16,mailbox->identity,64);}
    return eg_stream_write(client,reply,sizeof(reply),eg_stream_now()+2000) && ok;
}
int eg_broker_serve(int listener,EgMailbox *mailbox,const uint8_t token[16],eg_uid_t uid,eg_gid_t gid,EgBrokerResult *result) {
    memset(result,0,sizeof(*result));int client=-1;uint64_t until=eg_stream_now()+30000;
    while(result->rejected<8 && eg_stream_now()<until) {
        if(!eg_stream_wait(listener,EG_POLLIN,until))break;
        client=accept(listener,NULL,NULL);if(client<0){if(errno==EINTR || errno==EG_EAGAIN)continue;break;}
        if(eg_stream_prepare(client) && authenticate(client,mailbox,token,uid,gid))break;
        close(client);client=-1;result->rejected++;
    }
    if(client<0)return 0;
    dprintf(1,"*** EGPU-BROKER: authenticated uid=%u gid=%u\n",(unsigned)uid,(unsigned)gid);
    uint8_t request[EGPU_PAGE],previous[EGPU_PAGE],reply[EGPU_PAGE];uint32_t last=0;
    while(eg_stream_read(client,request,sizeof(request),eg_stream_now()+30000)) {
        EgpuFrame frame,response;
        if(egpu_decode(request,EGPU_PAGE,&frame) || (frame.opcode&EGPU_RESPONSE))break;
        if(last && frame.seq==last && !memcmp(request,previous,EGPU_HEADER+frame.size))result->replayed++;
        else {
            if(last==UINT32_MAX || frame.seq!=last+1 || (!last && frame.opcode!=EGPU_HELLO))break;
            if(!eg_mailbox_exchange(mailbox,request,reply))break;
            if(egpu_decode(reply,EGPU_PAGE,&response) || egpu_u32(response.payload))break;
            memcpy(previous,request,EGPU_PAGE);last=frame.seq;result->forwarded++;
        }
        if(!eg_stream_write(client,reply,EGPU_PAGE,eg_stream_now()+5000))break;
        if(frame.opcode==EGPU_DESTROY){result->complete=1;break;}
    }
    close(client);
    dprintf(1,"*** EGPU-BROKER: finished complete=%d forwarded=%u replayed=%u rejected=%u\n",
        result->complete,result->forwarded,result->replayed,result->rejected);
    return result->complete;
}
