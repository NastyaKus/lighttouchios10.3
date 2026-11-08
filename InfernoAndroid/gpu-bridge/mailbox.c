/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mailbox.h"
#include "stream.h"
static int page(EgMailbox *m,int offset,uint8_t *bytes) {
    return lseek(m->fd,offset,0)==offset && read(m->fd,bytes,EGPU_PAGE)==EGPU_PAGE;
}
static int marked(EgMailbox *m) {
    return page(m,0,m->scratch) && !memcmp(m->scratch,EGPU_DISK_MAGIC,16) &&
        egpu_u32(m->scratch+16)==EGPU_VERSION && egpu_u32(m->scratch+20)==EGPU_DISK_BYTES;
}
static int file(EgMailbox *m,const char *name) {
    m->fd=open(name,O_RDONLY);if(m->fd<0)return 0;
    int ok=marked(m);close(m->fd);m->fd=-1;if(!ok)return 0;
    m->fd=open(name,O_RDWR);if(m->fd<0)return 0;
    if(!eg_stream_cloexec(m->fd,1) || !marked(m)){close(m->fd);m->fd=-1;return 0;}
    memcpy(m->identity,m->scratch,64);snprintf(m->device,sizeof(m->device),"%s",name);return 1;
}
int eg_mailbox_open(EgMailbox *m,const char *image) {
    memset(m,0,sizeof(*m));m->fd=-1;
    m->allocation=malloc(EGPU_PAGE*3+4095);if(!m->allocation)return 0;
    m->request=(uint8_t *)(((uintptr_t)m->allocation+4095)&~(uintptr_t)4095);
    m->reply=m->request+EGPU_PAGE;m->scratch=m->reply+EGPU_PAGE;
    if(image && file(m,image))return 1;
    if(!image)for(int attempt=0;attempt<60;attempt++) {
        for(int i=0;i<32;i++){char name[24];snprintf(name,sizeof(name),"/dev/rdisk%d",i);if(file(m,name))return 1;}
        usleep(1000000);
    }
    eg_mailbox_close(m);return 0;
}
int eg_mailbox_exchange(EgMailbox *m,const uint8_t request[EGPU_PAGE],uint8_t reply[EGPU_PAGE]) {
    EgpuFrame r;if(egpu_decode(request,EGPU_PAGE,&r) || (r.opcode&EGPU_RESPONSE))return 0;
    if(!marked(m) || memcmp(m->identity,m->scratch,64))return 0;
    memcpy(m->request,request,EGPU_PAGE);
    if(lseek(m->fd,EGPU_REQUEST_OFFSET,0)!=EGPU_REQUEST_OFFSET || write(m->fd,m->request,EGPU_PAGE)!=EGPU_PAGE || fsync(m->fd))return 0;
    uint64_t until=eg_stream_now()+30000;
    while(eg_stream_now()<until) {
        EgpuFrame response;
        if(page(m,EGPU_REPLY_OFFSET,m->reply) && !egpu_decode(m->reply,EGPU_PAGE,&response) &&
           response.seq==r.seq && response.opcode==(r.opcode|EGPU_RESPONSE) && response.size>=4) {
            memcpy(reply,m->reply,EGPU_PAGE);return 1;
        }
        usleep(10000);
    }
    return 0;
}
void eg_mailbox_close(EgMailbox *m) { if(m->fd>=0)close(m->fd);free(m->allocation);m->allocation=NULL;m->fd=-1; }
