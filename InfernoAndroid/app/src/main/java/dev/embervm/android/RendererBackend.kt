// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.File

enum class RendererBackend(val nativeId: Int) {
    CPU(0), GLES3(1);
    companion object {
        fun parse(value: String?) = entries.firstOrNull { it.name == value } ?: CPU
    }
}

/** A lost process is not proof of a GPU bug. Still, retry the next boot on CPU.
 * Only a clean QEMU exit or the user's explicit retry clears the latch. */
class GraphicsSafety(private val marker: File) {
    val interrupted get() = marker.exists()
    fun select(requested: RendererBackend, restoring: Boolean = false): RendererBackend {
        if (restoring || requested == RendererBackend.CPU || interrupted) return RendererBackend.CPU
        // Fail closed if the latch cannot be recorded (e.g. full storage).
        if (runCatching {
            marker.parentFile?.mkdirs()
            marker.writeText("GLES3 session ${System.currentTimeMillis()}\n")
        }.isFailure) return RendererBackend.CPU
        return RendererBackend.GLES3
    }
    fun cleanExit(status: Int) { if (status == 0) marker.delete() }
    fun retry() { marker.delete() }
}
