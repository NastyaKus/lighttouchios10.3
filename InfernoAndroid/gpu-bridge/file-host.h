/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include "presentation.h"
typedef struct {
    int success;
    uint64_t commands, draws, pixels_read;
    uint64_t api_calls, api_batches, api_draws, shader_compiles;
    EgPresentStats presentation;
    char vendor[128], renderer[192], version[128], error[256];
} EgpuHostResult;
/* BLOCKING. Dedicated worker in an isolated process with no other EGL users.
 * Creates a NEW diagnostic image with O_EXCL; never opens an existing disk.
 * GL driver calls themselves cannot be interrupted by this polling deadline. */
int egpu_file_host(const char *new_image,uint32_t timeout_ms,int (*cancelled)(void *),void *opaque,EgpuHostResult *result);
int egpu_file_host_ex(const char *new_image,uint32_t timeout_ms,int (*cancelled)(void *),void *opaque,EgpuHostResult *result,const EgPresentOptions *options);
