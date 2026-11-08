// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import org.junit.Assert.*
import org.junit.Test

class GpuPresentationTest {
    private val values=longArrayOf(60,60,1234,1,0,60,0xffff0000,0xffffffff,0xff8a7766,0xff00ff00,64,64,240,0)
    private val stats=GpuPresentationStats.decode(values)
    private val console="*** EGPU-PRESENT: PASS frames=60 window_frames=60 checks=191 shared-renderbuffer GPU blit"
    private val capture=GpuDisplayCapture("run-current",1234,30,0,stats.bitmapCorners,stats.bitmapCorners)
    private fun rejects(action:()->Unit){try{action();fail("Expected rejection")}catch(_:IllegalArgumentException){}catch(_:IllegalStateException){} }
    @Test fun convertsArgbAndGlOriginWithoutSwappingRedAndBlue() {
        assertEquals(0xff0000ffL,GpuPresentationEvidence.rgba(0xffff0000.toInt()))
        assertEquals(listOf(0xff8a7766L,0xff00ff00L,0xffff0000L,0xffffffffL),stats.bitmapCorners)
    }
    @Test fun acceptsCompleteWindowAndPixelCopyEvidence() { GpuPresentationEvidence.verify(console,stats,"run-current",capture) }
    @Test fun headlessBlitsAndMissingPixelCopyCannotClaimAnAndroidWindow() {
        rejects { GpuPresentationEvidence.verify(console,stats.copy(windowFrames=0),"run-current",capture) }
        rejects { GpuPresentationEvidence.verify(console,stats,"run-current",capture.copy(result=3)) }
        rejects { GpuPresentationEvidence.verify(console,stats,"run-current",capture.copy(observed=emptyList())) }
    }
    @Test fun rejectsStaleRunSurfaceFrameAndPreviousSessionColor() {
        rejects { GpuPresentationEvidence.verify(console,stats,"other-run",capture) }
        rejects { GpuPresentationEvidence.verify(console,stats,"run-current",capture.copy(generation=1233)) }
        rejects { GpuPresentationEvidence.verify(console,stats,"run-current",capture.copy(serial=61)) }
        rejects { GpuPresentationEvidence.verify(console,stats,"run-current",capture.copy(serial=0)) }
        rejects { GpuPresentationEvidence.verify(console,stats,"run-current",capture.copy(observed=listOf(0xff112233L)+capture.observed.drop(1))) }
    }
    @Test fun requiresGuestEvidenceAndChecksRgbaWithinTwoLevels() {
        rejects { GpuPresentationEvidence.verify("",stats,"run-current",capture) }
        rejects { GpuPresentationEvidence.verify(console+"\n*** EGPU-PRESENT: FAIL",stats,"run-current",capture) }
        assertTrue(GpuPresentationEvidence.pixelsMatch(listOf(0xff101010L,0L,0L,0L),listOf(0xff121212L,0L,0L,0L)))
        assertFalse(GpuPresentationEvidence.pixelsMatch(listOf(0xff101010L,0L,0L,0L),listOf(0xff131313L,0L,0L,0L)))
    }
    @Test fun validatesNativeShapeBoundsAndCpuUploadAccounting() {
        rejects { GpuPresentationStats.decode(values.copyOfRange(0,13)) }
        rejects { GpuPresentationStats.decode(values.copyOf().also { it[0]=Long.MAX_VALUE }) }
        rejects { GpuPresentationStats.decode(values.copyOf().also { it[1]=61 }) }
        rejects { GpuPresentationStats.decode(values.copyOf().also { it[6]=0x100000000 }) }
        rejects { GpuPresentationStats.decode(values.copyOf().also { it[12]=239 }) }
        rejects { GpuPresentationStats.decode(values.copyOf().also { it[13]=16384 }) }
    }
}
