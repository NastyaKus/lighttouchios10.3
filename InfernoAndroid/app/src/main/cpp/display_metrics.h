/* SPDX-License-Identifier: AGPL-3.0-or-later; optional engine ABI v1. */
#pragma once
#include <stdint.h>
/* Optional, cumulative ABI. A read never consumes FPS counters. All times are ns.
 * Histogram bins are 1 ms wide, final bin includes everything >=127 ms.
 * main_cpu_ns samples CLOCK_THREAD_CPUTIME_ID on the QEMU main loop, not vCPUs.
 */
typedef struct InfernoDisplayMetricsV1 {
    uint32_t version, size;
    uint64_t presents, refreshes, pixman_cpu_ns, copy_cpu_ns, copy_wait_ns;
    uint64_t copy_bytes, main_cpu_ns, gap_count, gap_sum_ns, gap_max_ns;
    uint64_t gap_hist[128];
} InfernoDisplayMetricsV1;
