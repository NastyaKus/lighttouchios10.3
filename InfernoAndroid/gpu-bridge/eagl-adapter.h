/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gles2-subset.h"
/* Explicit opt-in, dedicated diagnostic process only. Registers our own
 * EAGLContext class iff Apple's class is absent; never swizzles a native class.
 * API=2, one offscreen context, no sharing or CAEAGLLayer support yet. */
int egEAGLInstall(const char *image,uint32_t width,uint32_t height);
int egEAGLInstallBroker(const char *socket,int capability_fd,uint32_t width,uint32_t height);
const char *egEAGLError(void);
EgGlesClient *egEAGLClient(void *object);
/* Retained diagnostic drawable object, deliberately NOT a CAEAGLLayer. */
void *egEAGLCreateDiagnosticDrawable(uint32_t width,uint32_t height);
