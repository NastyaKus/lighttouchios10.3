/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "executor.h"
#include "gles-host.h"
#include <string.h>
#include <stdlib.h>

static void set_error(EgGlesHost *g,GLenum error) { if(!g->error)g->error=error; }
static void collect_error(EgGlesHost *g) { GLenum err;while((err=glGetError()))set_error(g,err); }
static EgObject *object(EgGlesHost *g,uint32_t id,uint32_t kind) {
    if(!id || id>=EG_MAX_OBJECTS || g->objects[id].kind!=kind)return NULL;
    return &g->objects[id];
}
static int words(uint32_t n,uint32_t count) { return n==count*4; }
static GLuint framebuffer(EgpuExecutor *e) {
    EgObject *f=object(&e->gles,e->gles.framebuffer,EG_FRAMEBUFFER);
    return f?f->name:e->framebuffer;
}
static int dimensions(EgpuExecutor *e,uint32_t *w,uint32_t *h) {
    if(!e->gles.framebuffer){*w=e->width;*h=e->height;return 1;}
    EgObject *f=object(&e->gles,e->gles.framebuffer,EG_FRAMEBUFFER);
    EgObject *r=f?object(&e->gles,f->attachment,EG_RENDERBUFFER):NULL;
    if(!r)return 0;
    *w=r->width;*h=r->height;return *w && *h;
}
static int name(const uint8_t *p,uint32_t n,uint32_t prefix,char out[129]) {
    if(n<prefix+4)return 0;
    uint32_t len=egpu_u32(p+prefix);
    if(!len || len>128 || n!=prefix+4+len || memchr(p+prefix+4,0,len))return 0;
    memcpy(out,p+prefix+4,len);out[len]=0;return 1;
}
static int call(EgpuExecutor *e,uint16_t op,const uint8_t *p,uint32_t n,int execute) {
    EgGlesHost *g=&e->gles;EgObject *a,*b;uint32_t id,kind;char text[129];
    /* The validation pass must not change GL or object state. Resource and GL
     * state checks occur in execution order, after structural validation. */
    switch(op) {
    case EG_NEW:case EG_TEX_PARAMETER:case EG_DRAW:if(!words(n,3))return EGPU_BAD_ARGUMENT;
        break;
    case EG_DELETE:case EG_ATTACH:case EG_BIND_BUFFER:case EG_ATTRIB_ENABLE:
    case EG_BIND_TEXTURE:case EG_UNIFORM1I:case EG_BLEND_FUNC:case EG_ENABLE:
    case EG_BIND_RBO:case EG_BIND_FBO:if(!words(n,2))return EGPU_BAD_ARGUMENT;
        break;
    case EG_COMPILE:case EG_LINK:case EG_USE:case EG_CLEAR:case EG_ACTIVE_TEXTURE:if(!words(n,1))return EGPU_BAD_ARGUMENT;
        break;
    case EG_ATTRIB_POINTER:if(!words(n,6))return EGPU_BAD_ARGUMENT;
        break;
    case EG_VIEWPORT:case EG_CLEAR_COLOR:case EG_SCISSOR:case EG_RBO_STORAGE:case EG_FBO_ATTACH:if(!words(n,4))return EGPU_BAD_ARGUMENT;
        break;
    case EG_UNIFORM4F:if(!words(n,5))return EGPU_BAD_ARGUMENT;
        break;
    case EG_SOURCE:
        if(n<8 || egpu_u32(p+4)>EG_MAX_SOURCE || n!=8+egpu_u32(p+4))return EGPU_BAD_ARGUMENT;
        break;
    case EG_BUFFER_DATA:
        if(n<12 || egpu_u32(p+8)>EG_MAX_BUFFER || n!=12+egpu_u32(p+8))return EGPU_BAD_ARGUMENT;
        break;
    case EG_ATTRIB_LOCATION:if(!name(p,n,8,text))return EGPU_BAD_ARGUMENT;
        break;
    case EG_TEX_IMAGE:
        if(n<36 || egpu_u32(p+12)>EG_MAX_TEX_SIDE || egpu_u32(p+16)>EG_MAX_TEX_SIDE ||
           egpu_u32(p+32)!=(uint64_t)egpu_u32(p+12)*egpu_u32(p+16)*4 || n!=36+egpu_u32(p+32))return EGPU_BAD_ARGUMENT;
        break;
    default:return EGPU_BAD_COMMAND;
    }
    if(!execute)return EGPU_OK;
    g->calls++;
    if(!g->vao)glGenVertexArrays(1,&g->vao);
    glBindVertexArray(g->vao);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer(e));
    switch(op) {
    case EG_NEW:
        kind=egpu_u32(p);id=egpu_u32(p+4);
        if(!id || id>=EG_MAX_OBJECTS || g->objects[id].kind){set_error(g,GL_INVALID_VALUE);break;}
        a=&g->objects[id];
        if(kind==EG_SHADER) {
            GLenum type=egpu_u32(p+8);
            if(type!=GL_VERTEX_SHADER && type!=GL_FRAGMENT_SHADER){set_error(g,GL_INVALID_ENUM);break;}
            a->name=glCreateShader(type);
        } else if(kind==EG_PROGRAM)a->name=glCreateProgram();
        else if(kind==EG_BUFFER)glGenBuffers(1,&a->name);
        else if(kind==EG_TEXTURE)glGenTextures(1,&a->name);
        else if(kind==EG_RENDERBUFFER)glGenRenderbuffers(1,&a->name);
        else if(kind==EG_FRAMEBUFFER)glGenFramebuffers(1,&a->name);
        else {set_error(g,GL_INVALID_ENUM);break;}
        if(a->name)a->kind=kind;
        break;
    case EG_DELETE:
        kind=egpu_u32(p);id=egpu_u32(p+4);a=object(g,id,kind);
        if(!a){set_error(g,GL_INVALID_VALUE);break;}
        if(kind==EG_SHADER)glDeleteShader(a->name);
        else if(kind==EG_PROGRAM)glDeleteProgram(a->name);
        else if(kind==EG_BUFFER) {
            glDeleteBuffers(1,&a->name);if(g->array_buffer==id)g->array_buffer=0;
            for(unsigned i=0;i<EG_MAX_ATTRIBS;i++)if(g->attributes[i].object==id)g->attributes[i].object=0;
        } else if(kind==EG_TEXTURE) {
            glDeleteTextures(1,&a->name);for(int i=0;i<8;i++)if(g->textures[i]==id)g->textures[i]=0;
        } else if(kind==EG_RENDERBUFFER) {
            glDeleteRenderbuffers(1,&a->name);if(g->renderbuffer==id)g->renderbuffer=0;
        } else if(kind==EG_FRAMEBUFFER) {
            glDeleteFramebuffers(1,&a->name);if(g->framebuffer==id)g->framebuffer=0;
        }
        memset(a,0,sizeof(*a));break;
    case EG_SOURCE:
        a=object(g,egpu_u32(p),EG_SHADER);if(!a){set_error(g,GL_INVALID_VALUE);break;}
        {const char *source=(const char *)p+8;GLint len=egpu_u32(p+4);glShaderSource(a->name,1,&source,&len);}break;
    case EG_COMPILE:
        a=object(g,egpu_u32(p),EG_SHADER);if(!a){set_error(g,GL_INVALID_VALUE);break;}
        glCompileShader(a->name);g->shader_compiles++;break;
    case EG_ATTACH:
        a=object(g,egpu_u32(p),EG_PROGRAM);b=object(g,egpu_u32(p+4),EG_SHADER);
        if(!a || !b){set_error(g,GL_INVALID_VALUE);break;}glAttachShader(a->name,b->name);break;
    case EG_ATTRIB_LOCATION:
        a=object(g,egpu_u32(p),EG_PROGRAM);if(!a){set_error(g,GL_INVALID_VALUE);break;}
        if(egpu_u32(p+4)>=EG_MAX_ATTRIBS){set_error(g,GL_INVALID_VALUE);break;}
        name(p,n,8,text);glBindAttribLocation(a->name,egpu_u32(p+4),text);break;
    case EG_LINK:
        a=object(g,egpu_u32(p),EG_PROGRAM);if(!a){set_error(g,GL_INVALID_VALUE);break;}glLinkProgram(a->name);break;
    case EG_USE:
        id=egpu_u32(p);a=object(g,id,EG_PROGRAM);if(id && !a){set_error(g,GL_INVALID_VALUE);break;}
        glUseProgram(a?a->name:0);break;
    case EG_BIND_BUFFER:
        if(egpu_u32(p)!=GL_ARRAY_BUFFER){set_error(g,GL_INVALID_ENUM);break;}
        id=egpu_u32(p+4);a=object(g,id,EG_BUFFER);if(id && !a){set_error(g,GL_INVALID_VALUE);break;}
        glBindBuffer(GL_ARRAY_BUFFER,a?a->name:0);g->array_buffer=id;break;
    case EG_BUFFER_DATA:
        if(egpu_u32(p)!=GL_ARRAY_BUFFER){set_error(g,GL_INVALID_ENUM);break;}
        a=object(g,g->array_buffer,EG_BUFFER);if(!a){set_error(g,GL_INVALID_OPERATION);break;}
        glBufferData(GL_ARRAY_BUFFER,egpu_u32(p+8),p+12,egpu_u32(p+4));
        {GLenum err=glGetError();if(err)set_error(g,err);else a->bytes=egpu_u32(p+8);}
        break;
    case EG_ATTRIB_POINTER: {
        uint32_t index=egpu_u32(p),size=egpu_u32(p+4),stride=egpu_u32(p+16),offset=egpu_u32(p+20);
        a=object(g,g->array_buffer,EG_BUFFER);
        if(index>=EG_MAX_ATTRIBS || size<1 || size>4 || stride>255 || offset>EG_MAX_BUFFER || offset%4){set_error(g,GL_INVALID_VALUE);break;}
        if(egpu_u32(p+8)!=GL_FLOAT){set_error(g,GL_INVALID_ENUM);break;}
        if(!a){set_error(g,GL_INVALID_OPERATION);break;}
        glVertexAttribPointer(index,size,GL_FLOAT,egpu_u32(p+12)!=0,stride,(void *)(uintptr_t)offset);
        g->attributes[index].object=g->array_buffer;g->attributes[index].offset=offset;
        g->attributes[index].stride=stride?stride:size*4;g->attributes[index].size=size;break;
    }
    case EG_ATTRIB_ENABLE:
        id=egpu_u32(p);if(id>=EG_MAX_ATTRIBS){set_error(g,GL_INVALID_VALUE);break;}
        g->attributes[id].enabled=egpu_u32(p+4)!=0;
        if(egpu_u32(p+4))glEnableVertexAttribArray(id);else glDisableVertexAttribArray(id);break;
    case EG_VIEWPORT:glViewport((GLint)egpu_u32(p),(GLint)egpu_u32(p+4),(GLsizei)egpu_u32(p+8),(GLsizei)egpu_u32(p+12));break;
    case EG_SCISSOR:glScissor((GLint)egpu_u32(p),(GLint)egpu_u32(p+4),(GLsizei)egpu_u32(p+8),(GLsizei)egpu_u32(p+12));break;
    case EG_BIND_RBO:
        if(egpu_u32(p)!=GL_RENDERBUFFER){set_error(g,GL_INVALID_ENUM);break;}
        id=egpu_u32(p+4);a=object(g,id,EG_RENDERBUFFER);
        if(id && !a){set_error(g,GL_INVALID_VALUE);break;}
        glBindRenderbuffer(GL_RENDERBUFFER,a?a->name:0);g->renderbuffer=id;break;
    case EG_BIND_FBO:
        if(egpu_u32(p)!=GL_FRAMEBUFFER){set_error(g,GL_INVALID_ENUM);break;}
        id=egpu_u32(p+4);a=object(g,id,EG_FRAMEBUFFER);
        if(id && !a){set_error(g,GL_INVALID_VALUE);break;}
        g->framebuffer=id;glBindFramebuffer(GL_FRAMEBUFFER,framebuffer(e));break;
    case EG_RBO_STORAGE:
        a=object(g,g->renderbuffer,EG_RENDERBUFFER);
        if(!a){set_error(g,GL_INVALID_OPERATION);break;}
        if(egpu_u32(p)!=GL_RENDERBUFFER || egpu_u32(p+4)!=GL_RGBA8){set_error(g,GL_INVALID_ENUM);break;}
        if(!egpu_u32(p+8) || !egpu_u32(p+12) || egpu_u32(p+8)>512 || egpu_u32(p+12)>512){set_error(g,GL_INVALID_VALUE);break;}
        glBindRenderbuffer(GL_RENDERBUFFER,a->name);glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,egpu_u32(p+8),egpu_u32(p+12));
        {GLenum err=glGetError();if(err)set_error(g,err);else{a->width=egpu_u32(p+8);a->height=egpu_u32(p+12);}}
        break;
    case EG_FBO_ATTACH:
        a=object(g,g->framebuffer,EG_FRAMEBUFFER);
        if(!a){set_error(g,GL_INVALID_OPERATION);break;}
        if(egpu_u32(p)!=GL_FRAMEBUFFER || egpu_u32(p+4)!=GL_COLOR_ATTACHMENT0 || egpu_u32(p+8)!=GL_RENDERBUFFER){set_error(g,GL_INVALID_ENUM);break;}
        id=egpu_u32(p+12);b=object(g,id,EG_RENDERBUFFER);
        if(id && !b){set_error(g,GL_INVALID_VALUE);break;}
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,b?b->name:0);a->attachment=id;break;
    case EG_CLEAR_COLOR:glClearColor(eg_get_float(p),eg_get_float(p+4),eg_get_float(p+8),eg_get_float(p+12));break;
    case EG_CLEAR:glClear(egpu_u32(p));e->draws++;break;
    case EG_DRAW: {
        int32_t first=(int32_t)egpu_u32(p+4),count=(int32_t)egpu_u32(p+8);int valid=first>=0 && count>=0 && count<=1024;
        for(unsigned i=0;i<EG_MAX_ATTRIBS && valid && count;i++)if(g->attributes[i].enabled) {
            EgAttrib *v=&g->attributes[i];a=object(g,v->object,EG_BUFFER);
            uint64_t end=(uint64_t)(uint32_t)first+(uint32_t)count-1;
            if(!a || end>UINT32_MAX || v->offset+end*v->stride+v->size*4>a->bytes)valid=0;
        }
        if(!valid){set_error(g,GL_INVALID_OPERATION);break;}
        glDrawArrays(egpu_u32(p),first,count);g->draws++;e->draws++;break;
    }
    case EG_ACTIVE_TEXTURE:
        id=egpu_u32(p);if(id<GL_TEXTURE0 || id>=GL_TEXTURE0+8){set_error(g,GL_INVALID_ENUM);break;}
        glActiveTexture(id);g->texture_unit=id-GL_TEXTURE0;break;
    case EG_BIND_TEXTURE:
        if(egpu_u32(p)!=GL_TEXTURE_2D){set_error(g,GL_INVALID_ENUM);break;}
        id=egpu_u32(p+4);a=object(g,id,EG_TEXTURE);if(id && !a){set_error(g,GL_INVALID_VALUE);break;}
        glBindTexture(GL_TEXTURE_2D,a?a->name:0);g->textures[g->texture_unit]=id;break;
    case EG_TEX_PARAMETER:
        a=object(g,g->textures[g->texture_unit],EG_TEXTURE);if(!a){set_error(g,GL_INVALID_OPERATION);break;}
        glTexParameteri(egpu_u32(p),egpu_u32(p+4),(GLint)egpu_u32(p+8));break;
    case EG_TEX_IMAGE:
        a=object(g,g->textures[g->texture_unit],EG_TEXTURE);if(!a){set_error(g,GL_INVALID_OPERATION);break;}
        if(egpu_u32(p)!=GL_TEXTURE_2D || egpu_u32(p+8)!=GL_RGBA || egpu_u32(p+24)!=GL_RGBA || egpu_u32(p+28)!=GL_UNSIGNED_BYTE){set_error(g,GL_INVALID_ENUM);break;}
        if(egpu_u32(p+4) || egpu_u32(p+20)){set_error(g,GL_INVALID_VALUE);break;}
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,egpu_u32(p+12),egpu_u32(p+16),0,GL_RGBA,GL_UNSIGNED_BYTE,p+36);
        {GLenum err=glGetError();if(err)set_error(g,err);else {a->width=egpu_u32(p+12);a->height=egpu_u32(p+16);}}
        break;
    case EG_UNIFORM4F:glUniform4f((GLint)egpu_u32(p),eg_get_float(p+4),eg_get_float(p+8),eg_get_float(p+12),eg_get_float(p+16));break;
    case EG_UNIFORM1I:glUniform1i((GLint)egpu_u32(p),(GLint)egpu_u32(p+4));break;
    case EG_BLEND_FUNC:glBlendFunc(egpu_u32(p),egpu_u32(p+4));break;
    case EG_ENABLE:
        if(egpu_u32(p)!=GL_BLEND && egpu_u32(p)!=GL_CULL_FACE && egpu_u32(p)!=GL_DEPTH_TEST &&
           egpu_u32(p)!=GL_DITHER && egpu_u32(p)!=GL_POLYGON_OFFSET_FILL &&
           egpu_u32(p)!=GL_SAMPLE_ALPHA_TO_COVERAGE && egpu_u32(p)!=GL_SAMPLE_COVERAGE &&
           egpu_u32(p)!=GL_SCISSOR_TEST && egpu_u32(p)!=GL_STENCIL_TEST) {
            set_error(g,GL_INVALID_ENUM);break;
        }
        if(egpu_u32(p+4))glEnable(egpu_u32(p));else glDisable(egpu_u32(p));break;
    }
    collect_error(g);return EGPU_OK;
}
int eg_gles_host_command(EgpuExecutor *e,uint16_t op,const uint8_t *p,uint32_t n,uint8_t *out,uint32_t *out_size) {
    EgGlesHost *g=&e->gles;*out_size=4;
    if(!e->handle)return EGPU_BAD_HANDLE;
    if(op==EGPU_GLES_PRESENT) {
        if(n!=8)return EGPU_BAD_ARGUMENT;
        EgObject *r=object(g,egpu_u32(p),EG_RENDERBUFFER);uint32_t serial=egpu_u32(p+4);
        if(!r || !r->width || !r->height)return EGPU_BAD_HANDLE;
        if(!serial || serial!=g->present_serial+1 || g->present_serial==UINT32_MAX)return EGPU_SEQUENCE;
        g->present_serial=serial;
        int status=eg_present(&e->presenter,&e->context,r->name,r->width,r->height,serial);
        EgPresentStats *s=&e->presenter.stats;
        egpu_put32(out+4,status);egpu_put32(out+8,serial);egpu_put32(out+12,(uint32_t)s->frames);
        egpu_put32(out+16,(uint32_t)s->window_frames);egpu_put32(out+20,(uint32_t)s->generation);
        egpu_put32(out+24,r->width);egpu_put32(out+28,r->height);
        for(int i=0;i<4;i++)egpu_put32(out+32+i*4,s->pixels[i]);
        *out_size=48;return EGPU_OK;
    }
    if(op==EGPU_GLES_BATCH) {
        if(n<4)return EGPU_BAD_ARGUMENT;
        uint32_t count=egpu_u32(p),at=4;if(!count || count>128)return EGPU_BAD_ARGUMENT;
        /* Validate the entire batch before executing any of its commands. */
        for(uint32_t i=0;i<count;i++) {
            if(at>n || n-at<4)return EGPU_BAD_ARGUMENT;
            uint16_t cmd=egpu_u16(p+at),size=egpu_u16(p+at+2);at+=4;
            if(size>n-at)return EGPU_BAD_ARGUMENT;
            int status=call(e,cmd,p+at,size,0);if(status)return status;at+=size;
        }
        if(at!=n)return EGPU_BAD_ARGUMENT;
        at=4;for(uint32_t i=0;i<count;i++) {
            uint16_t cmd=egpu_u16(p+at),size=egpu_u16(p+at+2);at+=4;
            call(e,cmd,p+at,size,1);at+=size;
        }
        glFlush();collect_error(g);g->batches++;return EGPU_OK;
    }
    if(op!=EGPU_GLES_QUERY || n<12)return EGPU_BAD_ARGUMENT;
    uint32_t kind=egpu_u32(p),id=egpu_u32(p+4),param=egpu_u32(p+8);EgObject *a;GLint value=0;
    g->calls++;
    switch(kind) {
    case EG_ERROR_QUERY:
        if(n!=12)return EGPU_BAD_ARGUMENT;
        collect_error(g);value=g->error;g->error=0;break;
    case EG_SHADER_IV:case EG_PROGRAM_IV:
        if(n!=12)return EGPU_BAD_ARGUMENT;
        a=object(g,id,kind==EG_SHADER_IV?EG_SHADER:EG_PROGRAM);
        if(!a){set_error(g,GL_INVALID_VALUE);break;}
        /* GLES3 host queries such as COMPUTE_WORK_GROUP_SIZE write arrays.
         * Only scalar GLES2 pnames may use this one-integer reply slot. */
        if(kind==EG_SHADER_IV && param!=GL_SHADER_TYPE && param!=GL_COMPILE_STATUS &&
           param!=GL_DELETE_STATUS && param!=GL_INFO_LOG_LENGTH && param!=GL_SHADER_SOURCE_LENGTH) {
            set_error(g,GL_INVALID_ENUM);break;
        }
        if(kind==EG_PROGRAM_IV && param!=GL_LINK_STATUS && param!=GL_VALIDATE_STATUS &&
           param!=GL_DELETE_STATUS && param!=GL_ATTACHED_SHADERS && param!=GL_INFO_LOG_LENGTH &&
           param!=GL_ACTIVE_ATTRIBUTES && param!=GL_ACTIVE_ATTRIBUTE_MAX_LENGTH &&
           param!=GL_ACTIVE_UNIFORMS && param!=GL_ACTIVE_UNIFORM_MAX_LENGTH) {
            set_error(g,GL_INVALID_ENUM);break;
        }
        if(kind==EG_SHADER_IV)glGetShaderiv(a->name,param,&value);else glGetProgramiv(a->name,param,&value);break;
    case EG_ATTRIB_QUERY:case EG_UNIFORM_QUERY: {
        char key[129];if(!name(p,n,8,key))return EGPU_BAD_ARGUMENT;
        a=object(g,id,EG_PROGRAM);if(!a){set_error(g,GL_INVALID_VALUE);value=-1;break;}
        value=kind==EG_ATTRIB_QUERY?glGetAttribLocation(a->name,key):glGetUniformLocation(a->name,key);break;
    }
    case EG_LOG_QUERY: {
        if(n!=16 || param>1)return EGPU_BAD_ARGUMENT;
        a=object(g,id,param?EG_PROGRAM:EG_SHADER);
        if(!a){set_error(g,GL_INVALID_VALUE);break;}
        uint32_t capacity=egpu_u32(p+12);if(!capacity || capacity>1024)return EGPU_BAD_ARGUMENT;
        GLsizei got=0;
        if(param)glGetProgramInfoLog(a->name,capacity,&got,(char *)out+8);
        else glGetShaderInfoLog(a->name,capacity,&got,(char *)out+8);
        egpu_put32(out+4,got);*out_size=8+(uint32_t)got+1;collect_error(g);return EGPU_OK;
    }
    case EG_READ_RECT: {
        if(n!=20)return EGPU_BAD_ARGUMENT;
        uint32_t x=id,y=param,w=egpu_u32(p+12),h=egpu_u32(p+16);
        uint32_t fw,fh;
        if(!dimensions(e,&fw,&fh) || !w || !h || w>16 || h>16 || x>fw || y>fh || w>fw-x || h>fh-y)return EGPU_BAD_ARGUMENT;
        glBindFramebuffer(GL_FRAMEBUFFER,framebuffer(e));glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,out+4);*out_size=4+w*h*4;e->pixels_read+=(uint64_t)w*h;
        collect_error(g);return EGPU_OK;
    }
    case EG_INTEGER_QUERY:
        if(n!=12)return EGPU_BAD_ARGUMENT;
        if(param==GL_MAX_TEXTURE_SIZE)value=EG_MAX_TEX_SIDE;
        else if(param==GL_MAX_VERTEX_ATTRIBS)value=EG_MAX_ATTRIBS;
        else if(param==GL_MAX_TEXTURE_IMAGE_UNITS)value=8;
        else return EGPU_BAD_ARGUMENT;
        break;
    case EG_RBO_IV:
        if(n!=12)return EGPU_BAD_ARGUMENT;
        a=object(g,id,EG_RENDERBUFFER);
        if(!a || g->renderbuffer!=id){set_error(g,GL_INVALID_OPERATION);break;}
        if(param!=GL_RENDERBUFFER_WIDTH && param!=GL_RENDERBUFFER_HEIGHT && param!=GL_RENDERBUFFER_INTERNAL_FORMAT){set_error(g,GL_INVALID_ENUM);break;}
        glBindRenderbuffer(GL_RENDERBUFFER,a->name);glGetRenderbufferParameteriv(GL_RENDERBUFFER,param,&value);break;
    case EG_FBO_STATUS:
        if(n!=12 || param!=GL_FRAMEBUFFER)return EGPU_BAD_ARGUMENT;
        if(id!=g->framebuffer){set_error(g,GL_INVALID_OPERATION);break;}
        glBindFramebuffer(GL_FRAMEBUFFER,framebuffer(e));value=glCheckFramebufferStatus(GL_FRAMEBUFFER);break;
    default:return EGPU_BAD_ARGUMENT;
    }
    collect_error(g);egpu_put32(out+4,(uint32_t)value);*out_size=8;return EGPU_OK;
}
void eg_gles_host_close(EgGlesHost *g) {
    for(unsigned i=1;i<EG_MAX_OBJECTS;i++) {
        EgObject *a=&g->objects[i];
        if(a->kind==EG_SHADER)glDeleteShader(a->name);
        else if(a->kind==EG_PROGRAM)glDeleteProgram(a->name);
        else if(a->kind==EG_BUFFER)glDeleteBuffers(1,&a->name);
        else if(a->kind==EG_TEXTURE)glDeleteTextures(1,&a->name);
        else if(a->kind==EG_RENDERBUFFER)glDeleteRenderbuffers(1,&a->name);
        else if(a->kind==EG_FRAMEBUFFER)glDeleteFramebuffers(1,&a->name);
    }
    glDeleteVertexArrays(1,&g->vao);memset(g,0,sizeof(*g));
}
