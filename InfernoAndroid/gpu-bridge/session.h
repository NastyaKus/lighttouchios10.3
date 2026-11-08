/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "executor.h"
typedef struct {
    uint32_t last_seq;
    uint8_t request[EGPU_PAGE], reply[EGPU_PAGE];
    uint64_t replayed;
} EgpuSession;
int egpu_session_process(EgpuExecutor *e,EgpuSession *s,const uint8_t request[EGPU_PAGE],uint8_t reply[EGPU_PAGE]);
