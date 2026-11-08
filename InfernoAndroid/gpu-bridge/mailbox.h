/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "protocol.h"
typedef struct {
    int fd;
    void *allocation;
    uint8_t *request,*reply,*scratch,identity[64];
    char device[128];
} EgMailbox;
int eg_mailbox_open(EgMailbox *,const char *image);
int eg_mailbox_exchange(EgMailbox *,const uint8_t request[EGPU_PAGE],uint8_t reply[EGPU_PAGE]);
void eg_mailbox_close(EgMailbox *);
