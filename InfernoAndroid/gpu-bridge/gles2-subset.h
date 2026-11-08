/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public GLES ABI declarations for the implemented experimental subset.
 * This is not a complete or conformant GLES implementation. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned int GLenum,GLuint,GLbitfield;
typedef int GLint,GLsizei;
typedef unsigned char GLboolean,GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88e4
#define GL_DYNAMIC_DRAW 0x88e8
#define GL_VERTEX_SHADER 0x8b31
#define GL_FRAGMENT_SHADER 0x8b30
#define GL_COMPILE_STATUS 0x8b81
#define GL_LINK_STATUS 0x8b82
#define GL_INFO_LOG_LENGTH 0x8b84
#define GL_SHADER_SOURCE_LENGTH 0x8b88
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE_2D 0x0de1
#define GL_TEXTURE0 0x84c0
#define GL_RGBA 0x1908
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812f
#define GL_BLEND 0x0be2
#define GL_SCISSOR_TEST 0x0c11
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_ZERO 0
#define GL_PACK_ALIGNMENT 0x0d05
#define GL_UNPACK_ALIGNMENT 0x0cf5
#define GL_MAX_TEXTURE_SIZE 0x0d33
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_VERSION 0x1f02
#define GL_RENDERER 0x1f01
#define GL_VENDOR 0x1f00
#define GL_EXTENSIONS 0x1f03
#define GL_RENDERBUFFER 0x8d41
#define GL_FRAMEBUFFER 0x8d40
#define GL_COLOR_ATTACHMENT0 0x8ce0
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5
#define GL_RENDERBUFFER_WIDTH 0x8d42
#define GL_RENDERBUFFER_HEIGHT 0x8d43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8d44
#define GL_RENDERBUFFER_BINDING 0x8ca7
#define GL_FRAMEBUFFER_BINDING 0x8ca6
#define GL_RGBA8_OES 0x8058

/* Explicit connection/context setup; production EAGL integration is pending.
 * NULL image discovers ONLY a marked diagnostic raw namespace in iOS.
 * GL calls are thread-local, one current connection per calling thread. */
typedef struct EgGlesClient EgGlesClient;
EgGlesClient *egGlesOpen(const char *image,uint32_t width,uint32_t height);
EgGlesClient *egGlesOpenBroker(const char *socket,int capability_fd,uint32_t width,uint32_t height);
int egGlesMakeCurrent(EgGlesClient *context);
void egGlesClose(EgGlesClient *context);
const char *egGlesBackend(EgGlesClient *context);
uint64_t egGlesPackets(EgGlesClient *context);
/* Explicit diagnostic presentation, no Apple CAEAGLLayer ABI is claimed. */
int egGlesPresent(void);
void egGlesPresentSamples(EgGlesClient *,uint32_t rgba[4]);
uint32_t egGlesPresentedFrames(EgGlesClient *);
uint32_t egGlesWindowFrames(EgGlesClient *);
void egGlesChallenge(EgGlesClient *,uint8_t rgb[3]);
void glGenRenderbuffers(GLsizei n,GLuint *ids);
void glDeleteRenderbuffers(GLsizei n,const GLuint *ids);
void glBindRenderbuffer(GLenum target,GLuint id);
void glRenderbufferStorage(GLenum target,GLenum format,GLsizei w,GLsizei h);
void glGetRenderbufferParameteriv(GLenum target,GLenum pname,GLint *value);
void glGenFramebuffers(GLsizei n,GLuint *ids);
void glDeleteFramebuffers(GLsizei n,const GLuint *ids);
void glBindFramebuffer(GLenum target,GLuint id);
void glFramebufferRenderbuffer(GLenum target,GLenum attachment,GLenum rboTarget,GLuint rbo);
GLenum glCheckFramebufferStatus(GLenum target);
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader,GLsizei count,const GLchar *const *strings,const GLint *lengths);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader,GLenum pname,GLint *value);
void glGetShaderInfoLog(GLuint shader,GLsizei capacity,GLsizei *length,GLchar *log);
void glDeleteShader(GLuint shader);
GLuint glCreateProgram(void);
void glAttachShader(GLuint program,GLuint shader);
void glBindAttribLocation(GLuint program,GLuint index,const GLchar *name);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program,GLenum pname,GLint *value);
void glGetProgramInfoLog(GLuint program,GLsizei capacity,GLsizei *length,GLchar *log);
void glUseProgram(GLuint program);
void glDeleteProgram(GLuint program);
GLint glGetAttribLocation(GLuint program,const GLchar *name);
GLint glGetUniformLocation(GLuint program,const GLchar *name);
void glUniform4f(GLint location,GLfloat r,GLfloat g,GLfloat b,GLfloat a);
void glUniform1i(GLint location,GLint value);
void glGenBuffers(GLsizei count,GLuint *buffers);
void glBindBuffer(GLenum target,GLuint buffer);
void glBufferData(GLenum target,GLsizeiptr size,const void *data,GLenum usage);
void glDeleteBuffers(GLsizei count,const GLuint *buffers);
void glEnableVertexAttribArray(GLuint index);
void glDisableVertexAttribArray(GLuint index);
void glVertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void *offset);
void glViewport(GLint x,GLint y,GLsizei width,GLsizei height);
void glScissor(GLint x,GLint y,GLsizei width,GLsizei height);
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a);
void glClear(GLbitfield mask);
void glDrawArrays(GLenum mode,GLint first,GLsizei count);
void glGenTextures(GLsizei count,GLuint *textures);
void glDeleteTextures(GLsizei count,const GLuint *textures);
void glActiveTexture(GLenum unit);
void glBindTexture(GLenum target,GLuint texture);
void glTexParameteri(GLenum target,GLenum pname,GLint value);
void glTexImage2D(GLenum target,GLint level,GLint internalformat,GLsizei width,GLsizei height,GLint border,GLenum format,GLenum type,const void *pixels);
void glPixelStorei(GLenum pname,GLint value);
void glBlendFunc(GLenum source,GLenum destination);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glGetIntegerv(GLenum pname,GLint *value);
const GLubyte *glGetString(GLenum name);
void glReadPixels(GLint x,GLint y,GLsizei width,GLsizei height,GLenum format,GLenum type,void *pixels);
void glFlush(void);
void glFinish(void);
GLenum glGetError(void);
