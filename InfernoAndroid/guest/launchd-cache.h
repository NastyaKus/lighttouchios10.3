// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef EMBER_LAUNCHD_CACHE_H
#define EMBER_LAUNCHD_CACHE_H
#ifdef __APPLE__
#include "darwin-abi.h"
#else
#include <stddef.h>
#endif
/* Pure binary-plist editing: caller owns output, release with free().
 * All 5 labels + SpringBoard must exist, uniquely. Unknown objects preserved.
 * Returns 0 on success; no input bytes changed. Output is absent on failure. */
int lc_patch(const unsigned char *, size_t, unsigned char **, size_t *, const char **);
int lc_verify(const unsigned char *, size_t, const char **);
#endif
