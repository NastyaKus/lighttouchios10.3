/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gles2-subset.h"
int egProbePresentation(EgGlesClient *,void *opaque,int (*storage)(void *,uint32_t,uint32_t),int (*present)(void *));
