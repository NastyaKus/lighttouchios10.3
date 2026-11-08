// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

/** Native counters are cumulative and independent of the on-screen FPS reader. */
class GraphicsSample(val timeNs: Long, val values: LongArray) {
    init { require(values.size == 151 && values.all { it >= 0 }) }
    fun delta(previous: GraphicsSample): LongArray {
        require(timeNs > previous.timeNs)
        return LongArray(151) { i ->
            when(i) {
                8, 19 -> values[i] // ABI version and actual backend are gauges.
                17 -> 0 // Lifetime max cannot be subtracted. Use interval histogram.
                else -> (values[i] - previous.values[i]).coerceAtLeast(0)
            }
        }
    }
    companion object {
        val names = listOf("host_frames", "reads", "read_wall_ns", "present_cpu_ns", "present_wall_ns",
            "upload_bytes", "context_creates", "fallbacks", "engine_metrics_version", "pixman_cpu_ns",
            "copy_cpu_ns", "copy_wait_ns", "main_cpu_ns", "guest_frames", "refreshes", "gap_count",
            "gap_sum_ns", "lifetime_gap_max_ns", "copy_bytes", "actual_backend") + (0..127).map { "gap_bin_$it" } + listOf("swizzle_cpu_ns", "window_lock_wall_ns", "surface_changes")
        /** Upper bound in ms; null denotes an overflow bin (>=127ms) or no frames. */
        fun percentileUpperMs(delta: LongArray, percentile: Double): Int? {
            require(delta.size == 151 && percentile > 0 && percentile <= 1)
            val total = delta.sliceArray(20..147).sum()
            if (total == 0L) return null
            val target = kotlin.math.ceil(total * percentile).toLong()
            var count = 0L
            for (i in 0..127) {
                count += delta[20+i]
                if (count >= target) return if(i == 127) null else i+1
            }
            return null
        }
    }
}
