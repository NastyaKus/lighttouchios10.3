/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
/* Pass-through interposition on Apple's existing class, exact 18A5351d only.
 * No class registration, private ivar overwrite, ES3 advertisement or renderer
 * activation. This is the native interception layer for the pending frontend. */
enum { EG_NATIVE_INIT,EG_NATIVE_PROPERTIES,EG_NATIVE_COMMON,EG_NATIVE_API,
       EG_NATIVE_MACRO,EG_NATIVE_CURRENT,EG_NATIVE_SET_CURRENT,EG_NATIVE_HOOKS };
typedef struct { uint64_t calls[EG_NATIVE_HOOKS];unsigned installed; } EgNativeStats;
int egNativeEaglInstall(void);
/* Call only at a quiescent point. Does not overwrite somebody else's hook. */
int egNativeEaglRemove(void);
void egNativeEaglStats(EgNativeStats *out);
const char *egNativeEaglError(void);
