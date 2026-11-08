// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import org.junit.Assert.*
import org.junit.Test
import java.nio.file.Files

class GraphicsTest {
    @Test fun defaultsAndUnknownBackendKeepCpu() {
        assertEquals(RendererBackend.CPU, VmSettings().renderer)
        assertEquals(RendererBackend.CPU, RendererBackend.parse("vulkan"))
        assertEquals(RendererBackend.CPU, RendererBackend.parse(null))
        assertEquals(RendererBackend.GLES3, RendererBackend.parse("GLES3"))
    }
    @Test fun interruptedGpuRequiresExplicitRetryAndRestoreAlwaysUsesCpu() {
        val dir = Files.createTempDirectory("graphics-safety").toFile()
        try {
            val safety = GraphicsSafety(java.io.File(dir,"marker"))
            assertEquals(RendererBackend.CPU, safety.select(RendererBackend.GLES3, restoring=true))
            assertFalse(safety.interrupted)
            assertEquals(RendererBackend.GLES3, safety.select(RendererBackend.GLES3))
            assertTrue(safety.interrupted)
            assertEquals(RendererBackend.CPU, safety.select(RendererBackend.GLES3))
            safety.cleanExit(1); assertTrue(safety.interrupted)
            safety.retry(); assertFalse(safety.interrupted)
            assertEquals(RendererBackend.GLES3, safety.select(RendererBackend.GLES3))
            safety.cleanExit(0); assertFalse(safety.interrupted)
        } finally { dir.deleteRecursively() }
    }
    @Test fun unwritableSafetyLatchSelectsCpu() {
        val file = Files.createTempFile("graphics-parent", ".tmp").toFile()
        try {
            assertEquals(RendererBackend.CPU,
                GraphicsSafety(java.io.File(file,"marker")).select(RendererBackend.GLES3))
        } finally { file.delete() }
    }
    @Test fun diagnosticDeltasDoNotConsumeTotalsOrSubtractGauges() {
        val a=LongArray(151); a[0]=20; a[8]=1; a[19]=1; a[17]=99
        val b=a.copyOf(); b[0]=45; b[3]=700; b[17]=120
        val first=GraphicsSample(1,a); val last=GraphicsSample(2,b)
        val d=last.delta(first)
        assertEquals(25,d[0]); assertEquals(700,d[3]); assertEquals(1,d[8]); assertEquals(1,d[19])
        assertEquals(0,d[17]); assertEquals(45,b[0]); assertArrayEquals(d,last.delta(first))
    }
    @Test fun frameGapHistogramReportsUpperBoundsAndOverflow() {
        val d=LongArray(151); d[20+16]=95; d[20+127]=5
        assertEquals(17, GraphicsSample.percentileUpperMs(d,.50))
        assertEquals(17, GraphicsSample.percentileUpperMs(d,.95))
        assertNull(GraphicsSample.percentileUpperMs(d,.99))
        assertNull(GraphicsSample.percentileUpperMs(LongArray(151),.50))
    }
    @Test(expected=IllegalArgumentException::class) fun malformedSnapshotRejected() {
        GraphicsSample(0,LongArray(20))
    }
    @Test fun rendererChoiceNeverChangesGuestOrRestoreArguments() {
        val config=VmConfig(java.io.File("/kit"));val data=java.io.File("/qemu")
        val cpu=VmSettings(renderer=RendererBackend.CPU)
        assertEquals(config.arguments(cpu,data),config.arguments(cpu.copy(renderer=RendererBackend.GLES3),data))
        val ramdisk=java.io.File("/ramdisk")
        assertEquals(config.arguments(cpu,data,ramdisk),config.arguments(cpu.copy(renderer=RendererBackend.GLES3),data,ramdisk))
    }
}
