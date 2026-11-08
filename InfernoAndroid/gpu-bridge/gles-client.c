/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gles2-subset.h"
#include "gles-wire.h"
#include "stream.h"
#ifndef EGPU_GUEST_FREESTANDING
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#endif

struct EgGlesClient {
    int fd,broken,broker;
    int owner;
    void *allocation;
    uint8_t *request,*reply,*scratch;
    uint8_t identity[64],batch[EGPU_PAYLOAD];
    uint32_t seq,surface,width,height,next_id,batch_size,batch_count;
    uint32_t kinds[EG_MAX_OBJECTS];
    uint32_t object_width[EG_MAX_OBJECTS],object_height[EG_MAX_OBJECTS],attachments[EG_MAX_OBJECTS];
    uint32_t renderbuffer,framebuffer,present_serial,present_frames,window_frames,present_pixels[4],capabilities;
    GLint pack,unpack;
    GLenum error;
    uint64_t packets;
    char backend[512];
};
static _Thread_local EgGlesClient *current;
static _Thread_local GLenum no_context_error;
static void error(GLenum value) {
    GLenum *slot=current?&current->error:&no_context_error;if(!*slot)*slot=value;
}
static uint32_t bounded_length(const char *s,uint32_t maximum) {
    uint32_t n=0;if(!s)return maximum+1;
    while(n<=maximum && s[n])n++;
    return n;
}
static int page_read(int fd,uint32_t offset,uint8_t *data) {
    return lseek(fd,offset,0)==offset && read(fd,data,EGPU_PAGE)==EGPU_PAGE;
}
static int marked(int fd,EgGlesClient *c) {
    return page_read(fd,0,c->scratch) && !memcmp(c->scratch,EGPU_DISK_MAGIC,16) &&
        egpu_u32(c->scratch+16)==EGPU_VERSION && egpu_u32(c->scratch+20)==EGPU_DISK_BYTES;
}
static int connect_file(EgGlesClient *c,const char *path) {
    int fd=open(path,O_RDONLY);if(fd<0)return 0;
    int ok=marked(fd,c);close(fd);if(!ok)return 0;
    fd=open(path,O_RDWR);if(fd<0)return 0;
    if(!marked(fd,c)){close(fd);return 0;}
    c->fd=fd;memcpy(c->identity,c->scratch,64);return 1;
}
static int rpc(EgGlesClient *c,uint16_t op,const uint8_t *payload,uint32_t size,EgpuFrame *out) {
    if(c->broken || c->seq==UINT32_MAX)goto fail;
    if(!c->broker && (!marked(c->fd,c) || memcmp(c->identity,c->scratch,64)))goto fail;
    if(egpu_encode(c->request,op,++c->seq,payload,size))goto fail;
    if(c->broker) {
        c->packets++;uint64_t until=eg_stream_now()+30000;
        if(!eg_stream_write(c->fd,c->request,EGPU_PAGE,until) || !eg_stream_read(c->fd,c->reply,EGPU_PAGE,until) ||
           egpu_decode(c->reply,EGPU_PAGE,out) || out->opcode!=(op|EGPU_RESPONSE) || out->seq!=c->seq ||
           out->size<4 || egpu_u32(out->payload))goto fail;
        return 1;
    }
    if(lseek(c->fd,EGPU_REQUEST_OFFSET,0)!=EGPU_REQUEST_OFFSET || write(c->fd,c->request,EGPU_PAGE)!=EGPU_PAGE || fsync(c->fd))goto fail;
    c->packets++;
    for(int i=0;i<3000;i++) {
        if(page_read(c->fd,EGPU_REPLY_OFFSET,c->reply) && !egpu_decode(c->reply,EGPU_PAGE,out) &&
           out->opcode==(op|EGPU_RESPONSE) && out->seq==c->seq && out->size>=4) {
            if(egpu_u32(out->payload))goto fail;
            return 1;
        }
        usleep(10000);
    }
fail:
    c->broken=1;if(!c->error)c->error=GL_INVALID_OPERATION;return 0;
}
static int flush(EgGlesClient *c) {
    if(!c || c->broken)return 0;
    if(!c->batch_count)return 1;
    egpu_put32(c->batch,c->batch_count);EgpuFrame result;
    int ok=rpc(c,EGPU_GLES_BATCH,c->batch,c->batch_size,&result);
    c->batch_size=4;c->batch_count=0;return ok;
}
static int queue(uint16_t op,const uint8_t *p,uint32_t n) {
    EgGlesClient *c=current;if(!c){error(GL_INVALID_OPERATION);return 0;}
    if(n>EGPU_PAYLOAD-8){error(GL_OUT_OF_MEMORY);return 0;}
    if(c->batch_size+4+n>EGPU_PAYLOAD || c->batch_count==128)if(!flush(c))return 0;
    if(c->broken){error(GL_INVALID_OPERATION);return 0;}
    uint32_t at=c->batch_size;egpu_put16(c->batch+at,op);egpu_put16(c->batch+at+2,(uint16_t)n);
    if(n)memcpy(c->batch+at+4,p,n);
    c->batch_size+=4+n;c->batch_count++;return 1;
}
static int query(uint32_t kind,uint32_t id,uint32_t param,const uint8_t *extra,uint32_t n,EgpuFrame *out) {
    if(!current){error(GL_INVALID_OPERATION);return 0;}
    if(n>EGPU_PAYLOAD-12 || !flush(current))return 0;
    uint8_t p[EGPU_PAYLOAD];egpu_put32(p,kind);egpu_put32(p+4,id);egpu_put32(p+8,param);
    if(n)memcpy(p+12,extra,n);
    return rpc(current,EGPU_GLES_QUERY,p,12+n,out);
}
static void op1(uint16_t op,uint32_t a) { uint8_t p[4];egpu_put32(p,a);queue(op,p,4); }
static void op2(uint16_t op,uint32_t a,uint32_t b) { uint8_t p[8];egpu_put32(p,a);egpu_put32(p+4,b);queue(op,p,8); }
static int valid(GLuint id,uint32_t kind) {
    if(!current){error(GL_INVALID_OPERATION);return 0;}
    if(!id || id>=EG_MAX_OBJECTS || current->kinds[id]!=kind){error(GL_INVALID_VALUE);return 0;}
    return 1;
}
static GLuint new_object(uint32_t kind,uint32_t parameter) {
    if(!current){error(GL_INVALID_OPERATION);return 0;}
    if(current->next_id>=EG_MAX_OBJECTS){error(GL_OUT_OF_MEMORY);return 0;}
    GLuint id=current->next_id++;uint8_t p[12];egpu_put32(p,kind);egpu_put32(p+4,id);egpu_put32(p+8,parameter);
    if(!queue(EG_NEW,p,12))return 0;
    current->kinds[id]=kind;return id;
}
static void delete_object(GLuint id,uint32_t kind) {
    if(!id)return;
    if(valid(id,kind)){op2(EG_DELETE,kind,id);current->kinds[id]=0;}
}
static EgGlesClient *open_client(const char *path,int capability_fd,uint32_t width,uint32_t height) {
    if(current || !width || !height || width>512 || height>512)return NULL;
    EgGlesClient *c=malloc(sizeof(*c));if(!c)return NULL;memset(c,0,sizeof(*c));c->fd=-1;
    c->allocation=malloc(EGPU_PAGE*3+4095);if(!c->allocation){free(c);return NULL;}
    c->request=(uint8_t *)(((uintptr_t)c->allocation+4095)&~(uintptr_t)4095);
    c->reply=c->request+EGPU_PAGE;c->scratch=c->reply+EGPU_PAGE;
    if(capability_fd>=0) {
        uint8_t token[16];
        if(capability_fd<3 || !path || !eg_stream_read(capability_fd,token,16,eg_stream_now()+5000)){if(capability_fd>=3)close(capability_fd);goto fail;}
        close(capability_fd);c->fd=eg_stream_connect(path);c->broker=1;
        eg_uid_t uid=0;eg_gid_t gid=0;
#ifdef EGPU_GUEST_FREESTANDING
        eg_uid_t expected=0;
#else
        eg_uid_t expected=geteuid();
#endif
        int ok=c->fd>=0 && eg_stream_peer(c->fd,&uid,&gid) && uid==expected &&
            eg_broker_client_handshake(c->fd,token,c->identity);
        memset(token,0,sizeof(token));if(!ok)goto fail;
    }
    else if(path)connect_file(c,path);
    else for(int attempt=0;attempt<60 && c->fd<0;attempt++) {
        for(int i=0;i<32 && c->fd<0;i++) {
            char candidate[20]="/dev/rdisk";int at=10;
            if(i>=10)candidate[at++]=(char)('0'+i/10);
            candidate[at++]=(char)('0'+i%10);candidate[at]=0;connect_file(c,candidate);
        }
        if(c->fd<0)usleep(1000000);
    }
    if(c->fd<0)goto fail;
    EgpuFrame r;uint8_t p[8];
    if(!rpc(c,EGPU_HELLO,NULL,0,&r) || r.size<17 || egpu_u32(r.payload+4)!=EGPU_VERSION ||
       !(egpu_u32(r.payload+8)&EGPU_CAP_GLES_SUBSET) || r.payload[r.size-1])goto fail;
    uint32_t len=r.size-16;if(len>sizeof(c->backend))len=sizeof(c->backend);
    c->capabilities=egpu_u32(r.payload+8);
    memcpy(c->backend,r.payload+16,len);c->backend[sizeof(c->backend)-1]=0;
    egpu_put32(p,width);egpu_put32(p+4,height);
    if(!rpc(c,EGPU_CREATE,p,8,&r) || r.size!=8)goto fail;
    c->surface=egpu_u32(r.payload+4);c->width=width;c->height=height;c->next_id=1;c->batch_size=4;c->pack=c->unpack=4;
    c->owner=1;current=c;return c;
fail:
    if(c->fd>=0)close(c->fd);
    free(c->allocation);free(c);return NULL;
}
EgGlesClient *egGlesOpen(const char *path,uint32_t width,uint32_t height) { return open_client(path,-1,width,height); }
EgGlesClient *egGlesOpenBroker(const char *path,int capability_fd,uint32_t width,uint32_t height) {
    if(capability_fd<3)return NULL;
    return open_client(path,capability_fd,width,height);
}
int egGlesMakeCurrent(EgGlesClient *c) {
    if(c==current)return !c || !c->broken;
    if(c && __atomic_exchange_n(&c->owner,1,__ATOMIC_ACQ_REL))return 0;
    if(c && c->broken){__atomic_store_n(&c->owner,0,__ATOMIC_RELEASE);return 0;}
    if(current) {
        if(!flush(current)){if(c)__atomic_store_n(&c->owner,0,__ATOMIC_RELEASE);return 0;}
        __atomic_store_n(&current->owner,0,__ATOMIC_RELEASE);
    }
    current=c;return 1;
}
void egGlesClose(EgGlesClient *c) {
    if(!c)return;
    if(c!=current && __atomic_exchange_n(&c->owner,1,__ATOMIC_ACQ_REL)){error(GL_INVALID_OPERATION);return;}
    flush(c);
    if(c->surface && !c->broken){uint8_t p[4];EgpuFrame r;egpu_put32(p,c->surface);rpc(c,EGPU_DESTROY,p,4,&r);}
    if(current==c)current=NULL;
    close(c->fd);free(c->allocation);free(c);
}
const char *egGlesBackend(EgGlesClient *c) { return c?c->backend:""; }
uint64_t egGlesPackets(EgGlesClient *c) { return c?c->packets:0; }
GLuint glCreateShader(GLenum type) {
    if(type!=GL_VERTEX_SHADER && type!=GL_FRAGMENT_SHADER){error(GL_INVALID_ENUM);return 0;}
    return new_object(EG_SHADER,type);
}
void glShaderSource(GLuint shader,GLsizei count,const GLchar *const *strings,const GLint *lengths) {
    if(!valid(shader,EG_SHADER))return;
    if(count<0 || (count && !strings)){error(GL_INVALID_VALUE);return;}
    uint8_t p[EG_MAX_SOURCE+8];uint32_t used=0;egpu_put32(p,shader);
    for(GLsizei i=0;i<count;i++) {
        uint32_t n=lengths && lengths[i]>=0?(uint32_t)lengths[i]:bounded_length(strings[i],EG_MAX_SOURCE);
        if(n>EG_MAX_SOURCE-used || (n && !strings[i])){error(GL_INVALID_VALUE);return;}
        if(n)memcpy(p+8+used,strings[i],n);
        used+=n;
    }
    egpu_put32(p+4,used);queue(EG_SOURCE,p,8+used);
}
void glCompileShader(GLuint id) { if(valid(id,EG_SHADER))op1(EG_COMPILE,id); }
static void get_iv(uint32_t kind,uint32_t type,GLuint id,GLenum pname,GLint *value) {
    if(!value){error(GL_INVALID_VALUE);return;}*value=0;if(!valid(id,type))return;
    EgpuFrame r;if(query(kind,id,pname,NULL,0,&r) && r.size==8)*value=(GLint)egpu_u32(r.payload+4);
}
void glGetShaderiv(GLuint id,GLenum pname,GLint *value) { get_iv(EG_SHADER_IV,EG_SHADER,id,pname,value); }
void glGetProgramiv(GLuint id,GLenum pname,GLint *value) { get_iv(EG_PROGRAM_IV,EG_PROGRAM,id,pname,value); }
static void get_log(GLuint id,int program,GLsizei capacity,GLsizei *length,GLchar *log) {
    if(length)*length=0;
    if(capacity<0 || (capacity && !log)){error(GL_INVALID_VALUE);return;}
    if(!valid(id,program?EG_PROGRAM:EG_SHADER) || !capacity)return;
    log[0]=0;uint8_t p[4];uint32_t cap=capacity>1024?1024:(uint32_t)capacity;egpu_put32(p,cap);EgpuFrame r;
    if(query(EG_LOG_QUERY,id,program,p,4,&r) && r.size>=9) {
        uint32_t got=egpu_u32(r.payload+4);
        if(got<cap && r.size==9+got && !r.payload[8+got]){memcpy(log,r.payload+8,got+1);if(length)*length=got;}
        else error(GL_INVALID_OPERATION);
    }
}
void glGetShaderInfoLog(GLuint id,GLsizei capacity,GLsizei *length,GLchar *log) { get_log(id,0,capacity,length,log); }
void glGetProgramInfoLog(GLuint id,GLsizei capacity,GLsizei *length,GLchar *log) { get_log(id,1,capacity,length,log); }
void glDeleteShader(GLuint id) { delete_object(id,EG_SHADER); }
GLuint glCreateProgram(void) { return new_object(EG_PROGRAM,0); }
void glAttachShader(GLuint program,GLuint shader) { if(valid(program,EG_PROGRAM) && valid(shader,EG_SHADER))op2(EG_ATTACH,program,shader); }
void glBindAttribLocation(GLuint program,GLuint index,const GLchar *name) {
    if(!valid(program,EG_PROGRAM))return;
    uint32_t n=bounded_length(name,128);
    if(index>=EG_MAX_ATTRIBS || !n || n>128){error(GL_INVALID_VALUE);return;}
    uint8_t p[140];egpu_put32(p,program);egpu_put32(p+4,index);egpu_put32(p+8,n);memcpy(p+12,name,n);queue(EG_ATTRIB_LOCATION,p,12+n);
}
void glLinkProgram(GLuint id) { if(valid(id,EG_PROGRAM))op1(EG_LINK,id); }
void glUseProgram(GLuint id) { if(!id || valid(id,EG_PROGRAM))op1(EG_USE,id); }
void glDeleteProgram(GLuint id) { delete_object(id,EG_PROGRAM); }
static GLint location(uint32_t kind,GLuint id,const char *name) {
    if(!valid(id,EG_PROGRAM))return -1;
    uint32_t n=bounded_length(name,128);
    if(!n || n>128){error(GL_INVALID_VALUE);return -1;}
    EgpuFrame r;if(query(kind,id,n,(const uint8_t *)name,n,&r) && r.size==8)return (GLint)egpu_u32(r.payload+4);return -1;
}
GLint glGetAttribLocation(GLuint id,const GLchar *name) { return location(EG_ATTRIB_QUERY,id,name); }
GLint glGetUniformLocation(GLuint id,const GLchar *name) { return location(EG_UNIFORM_QUERY,id,name); }
void glUniform4f(GLint location,GLfloat r,GLfloat g,GLfloat b,GLfloat a) {
    uint8_t p[20];egpu_put32(p,(uint32_t)location);eg_put_float(p+4,r);eg_put_float(p+8,g);eg_put_float(p+12,b);eg_put_float(p+16,a);queue(EG_UNIFORM4F,p,20);
}
void glUniform1i(GLint location,GLint value) { op2(EG_UNIFORM1I,(uint32_t)location,(uint32_t)value); }
static void gen_objects(GLsizei n,GLuint *ids,uint32_t kind) {
    if(n<0 || (n && !ids)){error(GL_INVALID_VALUE);return;}
    for(GLsizei i=0;i<n;i++)ids[i]=new_object(kind,0);
}
static void delete_objects(GLsizei n,const GLuint *ids,uint32_t kind) {
    if(n<0 || (n && !ids)){error(GL_INVALID_VALUE);return;}
    for(GLsizei i=0;i<n;i++)delete_object(ids[i],kind);
}
void glGenBuffers(GLsizei n,GLuint *ids) { gen_objects(n,ids,EG_BUFFER); }
void glDeleteBuffers(GLsizei n,const GLuint *ids) { delete_objects(n,ids,EG_BUFFER); }
void glGenTextures(GLsizei n,GLuint *ids) { gen_objects(n,ids,EG_TEXTURE); }
void glDeleteTextures(GLsizei n,const GLuint *ids) { delete_objects(n,ids,EG_TEXTURE); }
void glGenRenderbuffers(GLsizei n,GLuint *ids) { gen_objects(n,ids,EG_RENDERBUFFER); }
void glGenFramebuffers(GLsizei n,GLuint *ids) { gen_objects(n,ids,EG_FRAMEBUFFER); }
void glDeleteRenderbuffers(GLsizei n,const GLuint *ids) {
    if(n<0 || (n && !ids)){error(GL_INVALID_VALUE);return;}
    for(GLsizei i=0;i<n;i++){if(current && current->renderbuffer==ids[i])current->renderbuffer=0;delete_object(ids[i],EG_RENDERBUFFER);}
}
void glDeleteFramebuffers(GLsizei n,const GLuint *ids) {
    if(n<0 || (n && !ids)){error(GL_INVALID_VALUE);return;}
    for(GLsizei i=0;i<n;i++){if(current && current->framebuffer==ids[i])current->framebuffer=0;delete_object(ids[i],EG_FRAMEBUFFER);}
}
void glBindRenderbuffer(GLenum target,GLuint id) {
    if(target!=GL_RENDERBUFFER){error(GL_INVALID_ENUM);return;}
    if(!current){error(GL_INVALID_OPERATION);return;}
    if(id && !valid(id,EG_RENDERBUFFER))return;
    op2(EG_BIND_RBO,target,id);current->renderbuffer=id;
}
void glBindFramebuffer(GLenum target,GLuint id) {
    if(target!=GL_FRAMEBUFFER){error(GL_INVALID_ENUM);return;}
    if(!current){error(GL_INVALID_OPERATION);return;}
    if(id && !valid(id,EG_FRAMEBUFFER))return;
    op2(EG_BIND_FBO,target,id);current->framebuffer=id;
}
void glRenderbufferStorage(GLenum target,GLenum format,GLsizei w,GLsizei h) {
    if(target!=GL_RENDERBUFFER || format!=GL_RGBA8_OES){error(GL_INVALID_ENUM);return;}
    if(w<=0 || h<=0 || w>512 || h>512){error(GL_INVALID_VALUE);return;}
    if(!current || !current->renderbuffer){error(GL_INVALID_OPERATION);return;}
    uint8_t p[16];egpu_put32(p,target);egpu_put32(p+4,format);egpu_put32(p+8,w);egpu_put32(p+12,h);
    if(queue(EG_RBO_STORAGE,p,16)){current->object_width[current->renderbuffer]=w;current->object_height[current->renderbuffer]=h;}
}
void glFramebufferRenderbuffer(GLenum target,GLenum attachment,GLenum rboTarget,GLuint rbo) {
    if(target!=GL_FRAMEBUFFER || attachment!=GL_COLOR_ATTACHMENT0 || rboTarget!=GL_RENDERBUFFER){error(GL_INVALID_ENUM);return;}
    if(!current || !current->framebuffer){error(GL_INVALID_OPERATION);return;}
    if(rbo && !valid(rbo,EG_RENDERBUFFER))return;
    uint8_t p[16];egpu_put32(p,target);egpu_put32(p+4,attachment);egpu_put32(p+8,rboTarget);egpu_put32(p+12,rbo);
    if(queue(EG_FBO_ATTACH,p,16))current->attachments[current->framebuffer]=rbo;
}
void glGetRenderbufferParameteriv(GLenum target,GLenum pname,GLint *value) {
    if(!value){error(GL_INVALID_VALUE);return;}*value=0;
    if(target!=GL_RENDERBUFFER){error(GL_INVALID_ENUM);return;}
    if(!current || !current->renderbuffer){error(GL_INVALID_OPERATION);return;}
    EgpuFrame r;if(query(EG_RBO_IV,current->renderbuffer,pname,NULL,0,&r) && r.size==8)*value=(GLint)egpu_u32(r.payload+4);
}
GLenum glCheckFramebufferStatus(GLenum target) {
    if(target!=GL_FRAMEBUFFER){error(GL_INVALID_ENUM);return 0;}
    if(!current){error(GL_INVALID_OPERATION);return 0;}
    EgpuFrame r;if(query(EG_FBO_STATUS,current->framebuffer,target,NULL,0,&r) && r.size==8)return egpu_u32(r.payload+4);return 0;
}
int egGlesPresent(void) {
    if(!current || !(current->capabilities&EGPU_CAP_PRESENT) || !current->renderbuffer || !current->object_width[current->renderbuffer] ||
       current->present_serial==UINT32_MAX || !flush(current))return 0;
    uint8_t p[8];egpu_put32(p,current->renderbuffer);egpu_put32(p+4,++current->present_serial);EgpuFrame r;
    if(!rpc(current,EGPU_GLES_PRESENT,p,8,&r) || r.size!=48)return 0;
    if(egpu_u32(r.payload+4))return 0;
    if(egpu_u32(r.payload+8)!=current->present_serial || egpu_u32(r.payload+12)!=current->present_frames+1 ||
       egpu_u32(r.payload+24)!=current->object_width[current->renderbuffer] ||
       egpu_u32(r.payload+28)!=current->object_height[current->renderbuffer]){error(GL_INVALID_OPERATION);return 0;}
    current->present_frames++;current->window_frames=egpu_u32(r.payload+16);
    for(int i=0;i<4;i++)current->present_pixels[i]=egpu_u32(r.payload+32+i*4);
    return 1;
}
void egGlesPresentSamples(EgGlesClient *c,uint32_t rgba[4]) { if(c && rgba)memcpy(rgba,c->present_pixels,sizeof(c->present_pixels)); }
uint32_t egGlesPresentedFrames(EgGlesClient *c) { return c?c->present_frames:0; }
uint32_t egGlesWindowFrames(EgGlesClient *c) { return c?c->window_frames:0; }
void egGlesChallenge(EgGlesClient *c,uint8_t rgb[3]) { if(c && rgb)for(int i=0;i<3;i++)rgb[i]=64+(c->identity[24+i]&127); }
void glBindBuffer(GLenum target,GLuint id) {
    if(target!=GL_ARRAY_BUFFER){error(GL_INVALID_ENUM);return;}if(!id || valid(id,EG_BUFFER))op2(EG_BIND_BUFFER,target,id);
}
void glBufferData(GLenum target,GLsizeiptr size,const void *data,GLenum usage) {
    if(target!=GL_ARRAY_BUFFER){error(GL_INVALID_ENUM);return;}
    if(size<0 || size>EG_MAX_BUFFER){error(GL_INVALID_VALUE);return;}
    uint8_t p[EG_MAX_BUFFER+12];egpu_put32(p,target);egpu_put32(p+4,usage);egpu_put32(p+8,(uint32_t)size);
    if(data)memcpy(p+12,data,(size_t)size);else memset(p+12,0,(size_t)size);queue(EG_BUFFER_DATA,p,12+(uint32_t)size);
}
void glEnableVertexAttribArray(GLuint index) { if(index>=EG_MAX_ATTRIBS){error(GL_INVALID_VALUE);return;}op2(EG_ATTRIB_ENABLE,index,1); }
void glDisableVertexAttribArray(GLuint index) { if(index>=EG_MAX_ATTRIBS){error(GL_INVALID_VALUE);return;}op2(EG_ATTRIB_ENABLE,index,0); }
void glVertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void *offset) {
    if(index>=EG_MAX_ATTRIBS || size<1 || size>4 || stride<0 || stride>255 || (uintptr_t)offset>EG_MAX_BUFFER || (uintptr_t)offset%4){error(GL_INVALID_VALUE);return;}
    if(type!=GL_FLOAT){error(GL_INVALID_ENUM);return;}
    uint8_t p[24];egpu_put32(p,index);egpu_put32(p+4,size);egpu_put32(p+8,type);egpu_put32(p+12,normalized!=0);egpu_put32(p+16,stride);egpu_put32(p+20,(uintptr_t)offset);queue(EG_ATTRIB_POINTER,p,24);
}
static void rectangle(uint16_t op,GLint x,GLint y,GLsizei w,GLsizei h) {
    if(w<0 || h<0){error(GL_INVALID_VALUE);return;}
    uint8_t p[16];egpu_put32(p,x);egpu_put32(p+4,y);egpu_put32(p+8,w);egpu_put32(p+12,h);queue(op,p,16);
}
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h) { rectangle(EG_VIEWPORT,x,y,w,h); }
void glScissor(GLint x,GLint y,GLsizei w,GLsizei h) { rectangle(EG_SCISSOR,x,y,w,h); }
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a) {
    uint8_t p[16];eg_put_float(p,r);eg_put_float(p+4,g);eg_put_float(p+8,b);eg_put_float(p+12,a);queue(EG_CLEAR_COLOR,p,16);
}
void glClear(GLbitfield mask) { op1(EG_CLEAR,mask); }
void glDrawArrays(GLenum mode,GLint first,GLsizei count) {
    if(first<0 || count<0 || count>1024){error(GL_INVALID_VALUE);return;}
    uint8_t p[12];egpu_put32(p,mode);egpu_put32(p+4,first);egpu_put32(p+8,count);queue(EG_DRAW,p,12);
}
void glActiveTexture(GLenum unit) { if(unit<GL_TEXTURE0 || unit>=GL_TEXTURE0+8){error(GL_INVALID_ENUM);return;}op1(EG_ACTIVE_TEXTURE,unit); }
void glBindTexture(GLenum target,GLuint id) {
    if(target!=GL_TEXTURE_2D){error(GL_INVALID_ENUM);return;}if(!id || valid(id,EG_TEXTURE))op2(EG_BIND_TEXTURE,target,id);
}
void glTexParameteri(GLenum target,GLenum pname,GLint value) {
    if(target!=GL_TEXTURE_2D){error(GL_INVALID_ENUM);return;}
    uint8_t p[12];egpu_put32(p,target);egpu_put32(p+4,pname);egpu_put32(p+8,value);queue(EG_TEX_PARAMETER,p,12);
}
void glTexImage2D(GLenum target,GLint level,GLint internalformat,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const void *pixels) {
    if(!current){error(GL_INVALID_OPERATION);return;}
    if(target!=GL_TEXTURE_2D || internalformat!=GL_RGBA || format!=GL_RGBA || type!=GL_UNSIGNED_BYTE){error(GL_INVALID_ENUM);return;}
    if(level || border || w<0 || h<0 || w>(GLsizei)EG_MAX_TEX_SIDE || h>(GLsizei)EG_MAX_TEX_SIDE){error(GL_INVALID_VALUE);return;}
    uint8_t p[36+EG_MAX_TEX_SIDE*EG_MAX_TEX_SIDE*4];uint32_t bytes=(uint32_t)w*h*4;
    egpu_put32(p,target);egpu_put32(p+4,level);egpu_put32(p+8,internalformat);egpu_put32(p+12,w);egpu_put32(p+16,h);
    egpu_put32(p+20,border);egpu_put32(p+24,format);egpu_put32(p+28,type);egpu_put32(p+32,bytes);
    uint32_t stride=((uint32_t)w*4+current->unpack-1)&~((uint32_t)current->unpack-1);
    for(GLsizei row=0;row<h;row++) {
        if(pixels)memcpy(p+36+(size_t)row*w*4,(const uint8_t *)pixels+(size_t)row*stride,(size_t)w*4);
        else memset(p+36+(size_t)row*w*4,0,(size_t)w*4);
    }
    queue(EG_TEX_IMAGE,p,36+bytes);
}
void glPixelStorei(GLenum pname,GLint value) {
    if(!current){error(GL_INVALID_OPERATION);return;}
    if(pname!=GL_PACK_ALIGNMENT && pname!=GL_UNPACK_ALIGNMENT){error(GL_INVALID_ENUM);return;}
    if(value!=1 && value!=2 && value!=4 && value!=8){error(GL_INVALID_VALUE);return;}
    if(pname==GL_PACK_ALIGNMENT)current->pack=value;else current->unpack=value;
}
void glBlendFunc(GLenum s,GLenum d) { op2(EG_BLEND_FUNC,s,d); }
void glEnable(GLenum cap) { op2(EG_ENABLE,cap,1); }
void glDisable(GLenum cap) { op2(EG_ENABLE,cap,0); }
void glGetIntegerv(GLenum pname,GLint *value) {
    if(!value){error(GL_INVALID_VALUE);return;}*value=0;
    if(pname==GL_PACK_ALIGNMENT || pname==GL_UNPACK_ALIGNMENT){if(current)*value=pname==GL_PACK_ALIGNMENT?current->pack:current->unpack;else error(GL_INVALID_OPERATION);return;}
    if(pname==GL_RENDERBUFFER_BINDING || pname==GL_FRAMEBUFFER_BINDING){if(current)*value=pname==GL_RENDERBUFFER_BINDING?current->renderbuffer:current->framebuffer;else error(GL_INVALID_OPERATION);return;}
    if(pname!=GL_MAX_TEXTURE_SIZE && pname!=GL_MAX_VERTEX_ATTRIBS && pname!=GL_MAX_TEXTURE_IMAGE_UNITS){error(GL_INVALID_ENUM);return;}
    EgpuFrame r;if(query(EG_INTEGER_QUERY,0,pname,NULL,0,&r) && r.size==8)*value=(GLint)egpu_u32(r.payload+4);
}
const GLubyte *glGetString(GLenum key) {
    if(!current){error(GL_INVALID_OPERATION);return NULL;}
    if(key==GL_VENDOR)return (const GLubyte *)"Ember experimental guest bridge";
    if(key==GL_RENDERER)return (const GLubyte *)current->backend;
    if(key==GL_VERSION)return (const GLubyte *)"Ember GLES2 API subset (not conformant)";
    if(key==GL_EXTENSIONS)return (const GLubyte *)"";
    error(GL_INVALID_ENUM);return NULL;
}
void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void *pixels) {
    if(!current){error(GL_INVALID_OPERATION);return;}
    if(format!=GL_RGBA || type!=GL_UNSIGNED_BYTE){error(GL_INVALID_ENUM);return;}
    uint32_t fw=current->width,fh=current->height;
    if(current->framebuffer){uint32_t rbo=current->attachments[current->framebuffer];fw=current->object_width[rbo];fh=current->object_height[rbo];}
    if(x<0 || y<0 || w<0 || h<0 || (uint32_t)x>fw || (uint32_t)y>fh ||
       (uint32_t)w>fw-(uint32_t)x || (uint32_t)h>fh-(uint32_t)y || (w && h && !pixels)){error(GL_INVALID_VALUE);return;}
    uint32_t stride=((uint32_t)w*4+current->pack-1)&~((uint32_t)current->pack-1);
    for(GLsizei row=0;row<h;row+=16)for(GLsizei col=0;col<w;col+=16) {
        uint32_t tw=w-col<16?w-col:16,th=h-row<16?h-row:16;uint8_t extra[8];egpu_put32(extra,tw);egpu_put32(extra+4,th);EgpuFrame r;
        if(!query(EG_READ_RECT,x+col,y+row,extra,8,&r) || r.size!=4+tw*th*4){error(GL_INVALID_OPERATION);return;}
        for(uint32_t line=0;line<th;line++)memcpy((uint8_t *)pixels+(row+line)*stride+col*4,r.payload+4+line*tw*4,tw*4);
    }
}
void glFlush(void) { if(!current)error(GL_INVALID_OPERATION);else flush(current); }
void glFinish(void) {
    if(!current){error(GL_INVALID_OPERATION);return;}
    if(flush(current)){uint8_t p[4];EgpuFrame r;egpu_put32(p,current->surface);rpc(current,EGPU_FENCE,p,4,&r);}
}
GLenum glGetError(void) {
    if(!current){GLenum e=no_context_error;no_context_error=0;return e;}
    if(!flush(current)){GLenum e=current->error;current->error=0;return e?e:GL_INVALID_OPERATION;}
    if(current->error){GLenum e=current->error;current->error=0;return e;}
    EgpuFrame r;if(query(EG_ERROR_QUERY,0,0,NULL,0,&r) && r.size==8)return egpu_u32(r.payload+4);
    return GL_INVALID_OPERATION;
}
