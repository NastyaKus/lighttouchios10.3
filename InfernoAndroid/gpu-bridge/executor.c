/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "executor.h"
#include <stdio.h>
#include <string.h>

static GLuint shader(GLenum kind,const char *source) {
    GLuint s=glCreateShader(kind);glShaderSource(s,1,&source,NULL);glCompileShader(s);
    GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){glDeleteShader(s);return 0;}return s;
}
int egpu_executor_open(EgpuExecutor *e) {
    memset(e,0,sizeof(*e));
    if(!ember_gl_open(&e->context,(EGLNativeWindowType)0,true,1,1)) return EGPU_GL_ERROR;
    const char *vs="#version 300 es\nlayout(location=0) in vec2 position;void main(){gl_Position=vec4(position,0,1);}";
    const char *fs="#version 300 es\nprecision highp float;uniform vec4 rgba;out vec4 color;void main(){color=rgba;}";
    GLuint v=shader(GL_VERTEX_SHADER,vs),f=shader(GL_FRAGMENT_SHADER,fs);
    if(!v || !f){glDeleteShader(v);glDeleteShader(f);return EGPU_GL_ERROR;}
    e->program=glCreateProgram();glAttachShader(e->program,v);glAttachShader(e->program,f);
    glLinkProgram(e->program);glDeleteShader(v);glDeleteShader(f);
    GLint ok;glGetProgramiv(e->program,GL_LINK_STATUS,&ok);if(!ok)return EGPU_GL_ERROR;
    glGenVertexArrays(1,&e->vao);glGenBuffers(1,&e->buffer);
    return glGetError()==GL_NO_ERROR ? EGPU_OK:EGPU_GL_ERROR;
}
static void destroy_surface(EgpuExecutor *e) {
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&e->framebuffer);glDeleteTextures(1,&e->color);
    e->framebuffer=e->color=0;e->width=e->height=e->handle=0;
}
void egpu_executor_close(EgpuExecutor *e) {
    if(e->context.context)eg_present_close(&e->presenter,&e->context);
    if(e->context.context!=EGL_NO_CONTEXT && eglGetCurrentContext()==e->context.context) {
        eg_gles_host_close(&e->gles);
        destroy_surface(e);glDeleteBuffers(1,&e->buffer);glDeleteVertexArrays(1,&e->vao);glDeleteProgram(e->program);
    }
    ember_gl_close(&e->context);
}
static int has_handle(EgpuExecutor *e,const EgpuFrame *r) {
    return e->handle && r->size>=4 && egpu_u32(r->payload)==e->handle;
}
static void color(GLfloat rgba[4],const uint8_t *p) { for(int i=0;i<4;i++)rgba[i]=p[i]/255.0f; }
int egpu_execute(EgpuExecutor *e,const EgpuFrame *r,uint8_t reply[EGPU_PAGE]) {
    uint8_t payload[EGPU_PAYLOAD]={0};uint32_t size=4;int status=EGPU_OK;
    if(!e || !r || !reply || (r->opcode&EGPU_RESPONSE))return EGPU_BAD_FRAME;
    switch(r->opcode) {
    case EGPU_HELLO:
        if(r->size){status=EGPU_BAD_ARGUMENT;break;}
        egpu_put32(payload+4,EGPU_VERSION);egpu_put32(payload+8,15|EGPU_CAP_GLES_SUBSET|EGPU_CAP_PRESENT);egpu_put32(payload+12,512);
        size=16+(uint32_t)snprintf((char *)payload+16,sizeof(payload)-16,"%s / %s / %s",
             e->context.vendor,e->context.renderer,e->context.version)+1;
        break;
    case EGPU_CREATE: {
        if(r->size!=8){status=EGPU_BAD_ARGUMENT;break;}
        uint32_t w=egpu_u32(r->payload),h=egpu_u32(r->payload+4);
        if(!w || !h || w>512 || h>512 || e->handle || e->next_handle==UINT32_MAX){status=EGPU_BAD_ARGUMENT;break;}
        glGenTextures(1,&e->color);glBindTexture(GL_TEXTURE_2D,e->color);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glGenFramebuffers(1,&e->framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,e->framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,e->color,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){destroy_surface(e);status=EGPU_GL_ERROR;break;}
        e->width=w;e->height=h;e->handle=++e->next_handle;egpu_put32(payload+4,e->handle);size=8;
        glViewport(0,0,w,h);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);break;
    }
    case EGPU_CLEAR: {
        if(r->size!=8){status=EGPU_BAD_ARGUMENT;break;}
        if(!has_handle(e,r)){status=EGPU_BAD_HANDLE;break;}
        GLfloat rgba[4];color(rgba,r->payload+4);glBindFramebuffer(GL_FRAMEBUFFER,e->framebuffer);
        glClearColor(rgba[0],rgba[1],rgba[2],rgba[3]);glClear(GL_COLOR_BUFFER_BIT);e->draws++;break;
    }
    case EGPU_TRIANGLE: {
        if(r->size!=32){status=EGPU_BAD_ARGUMENT;break;}
        if(!has_handle(e,r)){status=EGPU_BAD_HANDLE;break;}
        GLfloat vertices[6],rgba[4];int valid=1;
        for(int i=0;i<6;i++) {
            uint32_t word=egpu_u32(r->payload+4+4*i);memcpy(&vertices[i],&word,4);
            if(!(vertices[i]>=-1 && vertices[i]<=1))valid=0; /* Includes NaN. */
        }
        if(!valid){status=EGPU_BAD_ARGUMENT;break;}
        color(rgba,r->payload+28);glBindFramebuffer(GL_FRAMEBUFFER,e->framebuffer);glViewport(0,0,e->width,e->height);
        glUseProgram(e->program);glUniform4fv(glGetUniformLocation(e->program,"rgba"),1,rgba);
        glBindVertexArray(e->vao);glBindBuffer(GL_ARRAY_BUFFER,e->buffer);
        glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
        glDrawArrays(GL_TRIANGLES,0,3);e->draws++;break;
    }
    case EGPU_READ_POINTS: {
        if(r->size<8){status=EGPU_BAD_ARGUMENT;break;}
        if(!has_handle(e,r)){status=EGPU_BAD_HANDLE;break;}
        uint32_t count=egpu_u32(r->payload+4);
        if(!count || count>16 || r->size!=8+count*8){status=EGPU_BAD_ARGUMENT;break;}
        for(uint32_t i=0;i<count;i++)if(egpu_u32(r->payload+8+i*8)>=e->width || egpu_u32(r->payload+12+i*8)>=e->height)status=EGPU_BAD_ARGUMENT;
        if(status)break;
        glBindFramebuffer(GL_FRAMEBUFFER,e->framebuffer);glPixelStorei(GL_PACK_ALIGNMENT,1);egpu_put32(payload+4,count);size=8+4*count;
        for(uint32_t i=0;i<count;i++)glReadPixels(egpu_u32(r->payload+8+i*8),egpu_u32(r->payload+12+i*8),1,1,GL_RGBA,GL_UNSIGNED_BYTE,payload+8+4*i);
        e->pixels_read+=count;break;
    }
    case EGPU_FENCE: {
        if(r->size!=4){status=EGPU_BAD_ARGUMENT;break;}
        if(!has_handle(e,r)){status=EGPU_BAD_HANDLE;break;}
        GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        if(!fence){status=EGPU_GL_ERROR;break;}
        GLenum result=glClientWaitSync(fence,GL_SYNC_FLUSH_COMMANDS_BIT,500000000ULL);glDeleteSync(fence);
        if(result==GL_TIMEOUT_EXPIRED)status=EGPU_TIMEOUT;
        else if(result==GL_WAIT_FAILED)status=EGPU_GL_ERROR;
        break;
    }
    case EGPU_DESTROY:
        if(r->size!=4){status=EGPU_BAD_ARGUMENT;break;}
        if(!has_handle(e,r)){status=EGPU_BAD_HANDLE;break;}
        destroy_surface(e);break;
    default:
        if(r->opcode==EGPU_GLES_BATCH || r->opcode==EGPU_GLES_QUERY || r->opcode==EGPU_GLES_PRESENT) {
            status=eg_gles_host_command(e,r->opcode,r->payload,r->size,payload,&size);
        } else status=EGPU_BAD_COMMAND;
        break;
    }
    if(glGetError()!=GL_NO_ERROR)status=EGPU_GL_ERROR;
    if(status)size=4;
    egpu_put32(payload,status);e->commands++;
    return egpu_encode(reply,r->opcode|EGPU_RESPONSE,r->seq,payload,size);
}
