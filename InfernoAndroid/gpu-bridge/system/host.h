/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "surface.h"
#include "commands.h"
typedef struct EgsHost EgsHost;
typedef struct {
    uint8_t *data;
    uint32_t size;
} EgsReply;
/* All methods, including close, belong to one EGL worker. Each guest context
 * has its own GL state; explicitly shared contexts share object namespaces.
 * Transfer and presentation use separate real ES3 contexts. */
EgsHost *egs_host_open(const EgPresentOptions *, char error[192]);
void egs_host_close(EgsHost *);
const char *egs_host_error(EgsHost *);
const char *egs_host_renderer(EgsHost *);
uint32_t egs_host_texture_limit(EgsHost *);
int egs_host_create(EgsHost *, uint32_t context, uint32_t share, uint32_t width, uint32_t height);
int egs_host_destroy(EgsHost *, uint32_t context);
int egs_host_call(EgsHost *, uint32_t context, const uint8_t *command, uint32_t size, const uint8_t *upload,
                  uint32_t upload_size, EgsReply *);
int egs_host_present(EgsHost *, uint32_t context, uint64_t surface, uint64_t serial, int *present_status);
void egs_reply_close(EgsReply *);
