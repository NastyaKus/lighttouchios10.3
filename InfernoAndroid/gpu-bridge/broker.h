/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "mailbox.h"
#include "stream.h"
typedef struct { uint32_t forwarded,replayed,rejected;int complete; } EgBrokerResult;
int eg_broker_serve(int listener,EgMailbox *,const uint8_t token[16],eg_uid_t uid,eg_gid_t gid,EgBrokerResult *);
