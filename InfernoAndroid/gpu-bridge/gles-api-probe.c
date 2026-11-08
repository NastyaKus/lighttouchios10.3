/* SPDX-License-Identifier: GPL-3.0-or-later
 * A standard GLES-call test linked to our experimental client library.
 * It is not linked to Apple's OpenGLES.framework and is not an EAGL driver. */
#include "gles2-subset.h"
#ifdef EGPU_GUEST_FREESTANDING
#undef NULL
#include "../guest/darwin-abi.h"
typedef void *pthread_t;
extern int pthread_create(pthread_t *,const void *,void *(*)(void *),void *);
extern int pthread_join(pthread_t,void **);
#else
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#endif

static unsigned checks;
__attribute__((weak)) int egProbeConfigure(int argc,char **argv) { (void)argc;(void)argv;return 1; }
__attribute__((weak)) EgGlesClient *egProbeOpen(const char *image,uint32_t width,uint32_t height) { return egGlesOpen(image,width,height); }
__attribute__((weak)) void egProbeClose(EgGlesClient *client) { egGlesClose(client); }
__attribute__((weak)) int egProbeExtraChecks(EgGlesClient *client) { (void)client;return 1; }
#define CHECK(condition) do { checks++;if(!(condition)){dprintf(1,"*** EGPU-GLES2: FAIL line=%u check=%s gl_error=0x%x\n",__LINE__,#condition,glGetError());goto fail;} } while(0)
static GLuint compile(GLenum kind,const char *source) {
    GLuint shader=glCreateShader(kind);glShaderSource(shader,1,&source,NULL);glCompileShader(shader);
    GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok){char text[512];glGetShaderInfoLog(shader,sizeof(text),NULL,text);dprintf(1,"*** EGPU-GLES2: shader error %s\n",text);glDeleteShader(shader);return 0;}
    return shader;
}
static GLuint program(GLuint vertex,GLuint fragment,int textured) {
    GLuint p=glCreateProgram();glAttachShader(p,vertex);glAttachShader(p,fragment);
    glBindAttribLocation(p,0,"position");if(textured)glBindAttribLocation(p,1,"texcoord");
    glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){char text[512];glGetProgramInfoLog(p,sizeof(text),NULL,text);dprintf(1,"*** EGPU-GLES2: link error %s\n",text);glDeleteProgram(p);return 0;}
    return p;
}
static int rgba_at(const uint8_t *pixels,unsigned x,unsigned y,unsigned r,unsigned g,unsigned b,unsigned a) {
    const uint8_t *p=pixels+((size_t)y*64+x)*4;return p[0]==r && p[1]==g && p[2]==b && p[3]==a;
}
static void *other_thread(void *opaque) {
    int *ok=(int *)((void **)opaque)[1];EgGlesClient *c=((void **)opaque)[0];
    *ok=glGetError()==GL_NO_ERROR && !egGlesMakeCurrent(c);
    glClearColor(1,1,1,1);
    *ok=*ok && glGetError()==GL_INVALID_OPERATION && glGetError()==GL_NO_ERROR;
    return NULL;
}
int main(int argc,char **argv) {
    if(!egProbeConfigure(argc,argv)){dprintf(1,"*** EGPU-GLES2: FAIL client configuration\n");return 1;}
    const char *image=argc==3 && !strncmp(argv[1],"--image",8)?argv[2]:NULL;
    EgGlesClient *c=egProbeOpen(image,64,64);
    if(!c){dprintf(1,"*** EGPU-GLES2: FAIL cannot connect marked channel / create context\n");return 1;}
    dprintf(1,"*** EGPU-GLES2: backend %s\n",egGlesBackend(c));
    uint8_t pixels[64*64*4];GLint value=0;
    CHECK(glGetError()==GL_NO_ERROR);
    const char *vs="attribute vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}";
    const char *fs="precision mediump float;uniform vec4 rgba;void main(){gl_FragColor=rgba;}";
    GLuint vertex=compile(GL_VERTEX_SHADER,vs),fragment=compile(GL_FRAGMENT_SHADER,fs);
    CHECK(vertex && fragment);
    GLuint color_program=program(vertex,fragment,0);CHECK(color_program);
    CHECK(glGetAttribLocation(color_program,"position")==0);
    GLint color_location=glGetUniformLocation(color_program,"rgba");CHECK(color_location>=0);
    CHECK(glGetUniformLocation(color_program,"missing_uniform")==-1 && glGetError()==GL_NO_ERROR);
    glGetProgramiv(color_program,0x8267,&value); /* Host's multi-value compute pname is not GLES2. */
    CHECK(value==0 && glGetError()==GL_INVALID_ENUM && glGetError()==GL_NO_ERROR);
    GLuint bad=glCreateShader(GL_FRAGMENT_SHADER);const char *bad_source="not valid GLSL; }";
    glShaderSource(bad,1,&bad_source,NULL);glCompileShader(bad);glGetShaderiv(bad,GL_COMPILE_STATUS,&value);CHECK(value==GL_FALSE);
    glGetShaderiv(bad,GL_INFO_LOG_LENGTH,&value);CHECK(value>1);
    char log[512];GLsizei length=0;glGetShaderInfoLog(bad,sizeof(log),&length,log);CHECK(length>0 && log[length]==0);
    dprintf(1,"*** EGPU-GLES2: real compiler rejected bad shader (%d log bytes)\n",length);
    CHECK(glGetError()==GL_NO_ERROR);
    GLuint failed=glCreateProgram();glAttachShader(failed,vertex);glAttachShader(failed,bad);glLinkProgram(failed);
    glGetProgramiv(failed,GL_LINK_STATUS,&value);CHECK(value==GL_FALSE);glDeleteProgram(failed);glDeleteShader(bad);
    const GLfloat triangle[]={-.75f,-.75f,.75f,-.75f,0,.75f};GLuint buffer=0;
    glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);glBufferData(GL_ARRAY_BUFFER,sizeof(triangle),triangle,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);glEnableVertexAttribArray(0);
    glUseProgram(color_program);glUniform4f(color_location,1,0,0,1);glViewport(0,0,64,64);
    glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    CHECK(rgba_at(pixels,32,32,255,0,0,255));CHECK(rgba_at(pixels,0,0,0,0,255,255));CHECK(glGetError()==GL_NO_ERROR);
    uint64_t before=egGlesPackets(c);
    for(int i=0;i<10;i++)glClearColor(i/10.0f,0,0,1);
    glFlush();CHECK(egGlesPackets(c)==before+1);
    dprintf(1,"*** EGPU-GLES2: batched ten GL calls into one request\n");
    const char *tvs="attribute vec2 position;attribute vec2 texcoord;varying mediump vec2 uv;void main(){uv=texcoord;gl_Position=vec4(position,0.0,1.0);}";
    const char *tfs="precision mediump float;varying mediump vec2 uv;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,uv);}";
    GLuint tv=compile(GL_VERTEX_SHADER,tvs),tf=compile(GL_FRAGMENT_SHADER,tfs);CHECK(tv && tf);
    GLuint textured=program(tv,tf,1);CHECK(textured);
    CHECK(glGetAttribLocation(textured,"texcoord")==1);
    GLint sampler=glGetUniformLocation(textured,"image");CHECK(sampler>=0);
    const GLfloat quad[]={-1,-1,0,0,1,-1,1,0,-1,1,0,1,1,1,1,1};
    glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,0);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,(void *)8);
    glEnableVertexAttribArray(1);GLuint texture=0;glGenTextures(1,&texture);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    const uint8_t texels[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,texels);
    glUseProgram(textured);glUniform1i(sampler,0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    CHECK(rgba_at(pixels,8,8,255,0,0,255));CHECK(rgba_at(pixels,56,8,0,255,0,255));
    CHECK(rgba_at(pixels,8,56,0,0,255,255));CHECK(rgba_at(pixels,56,56,255,255,255,255));CHECK(glGetError()==GL_NO_ERROR);
    uint8_t padded[32];memset(padded,0x55,sizeof(padded));
    glPixelStorei(GL_PACK_ALIGNMENT,8);glReadPixels(0,0,3,2,GL_RGBA,GL_UNSIGNED_BYTE,padded);
    CHECK(padded[12]==0x55 && padded[15]==0x55 && padded[28]==0x55 && padded[31]==0x55);
    glPixelStorei(GL_PACK_ALIGNMENT,4);
    uint8_t padded_texture[32];memset(padded_texture,0x55,sizeof(padded_texture));
    for(int row=0;row<2;row++)for(int col=0;col<3;col++){uint8_t *p=padded_texture+row*16+col*4;p[0]=0;p[1]=255;p[2]=255;p[3]=255;}
    glPixelStorei(GL_UNPACK_ALIGNMENT,8);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,3,2,0,GL_RGBA,GL_UNSIGNED_BYTE,padded_texture);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    CHECK(rgba_at(pixels,8,8,0,255,255,255) && rgba_at(pixels,56,56,0,255,255,255));glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glUseProgram(color_program);glUniform4f(color_location,1,0,0,.5f);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,padded);
    CHECK(padded[0]>=127 && padded[0]<=128 && padded[1]==0 && padded[2]>=127 && padded[2]<=128 && padded[3]>=191 && padded[3]<=192);
    glDisable(GL_BLEND);glEnable(GL_SCISSOR_TEST);glScissor(0,0,32,64);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    CHECK(rgba_at(pixels,8,8,0,255,0,255));CHECK(!rgba_at(pixels,56,8,0,255,0,255));glDisable(GL_SCISSOR_TEST);
    CHECK(glGetError()==GL_NO_ERROR);
    glPixelStorei(GL_PACK_ALIGNMENT,3);CHECK(glGetError()==GL_INVALID_VALUE && glGetError()==GL_NO_ERROR);
    glBindBuffer(0xdead,buffer);CHECK(glGetError()==GL_INVALID_ENUM && glGetError()==GL_NO_ERROR);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,(void *)2048);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    CHECK(glGetError()==GL_INVALID_OPERATION && glGetError()==GL_NO_ERROR);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,0);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&value);CHECK(value==16);
    int thread_ok=0;void *thread_data[2]={c,&thread_ok};pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,other_thread,thread_data));CHECK(!pthread_join(thread,NULL) && thread_ok);
    CHECK(glGetError()==GL_NO_ERROR);
    glDisableVertexAttribArray(0);glDisableVertexAttribArray(1);glUseProgram(0);
    glDeleteBuffers(1,&buffer);glDeleteTextures(1,&texture);
    glDeleteProgram(color_program);glDeleteProgram(textured);glDeleteShader(vertex);glDeleteShader(fragment);glDeleteShader(tv);glDeleteShader(tf);
    glFinish();CHECK(glGetError()==GL_NO_ERROR);
    CHECK(egProbeExtraChecks(c));
    dprintf(1,"*** EGPU-GLES2: PASS checks=%u packets=%llu shaders/textures/buffers/blend/scissor/packing/TLS\n",checks,(unsigned long long)egGlesPackets(c));
    egProbeClose(c);return 0;
fail:
    egProbeClose(c);return 1;
}
