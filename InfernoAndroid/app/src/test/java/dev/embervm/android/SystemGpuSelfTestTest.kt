// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test

class SystemGpuSelfTestTest {
    private val secret=ByteArray(32){(it*3).toByte()};private val run=ByteArray(16){(it+11).toByte()}
    private fun passing(frames:Int)=SystemGpuSelfTest.parse("passed=1\nstage=done\nrenderer=Adreno (TM) 740\n" +
        "readback_bytes=${SystemGpuSelfTest.PANEL_BYTES}\npresents=$frames\npresented_to_window=$frames\n")
    private val good=List(4){ listOf(221,44,7,255) }

    @Test fun parsesKeyValueReportIncludingEqualsInValues() {
        val r=SystemGpuSelfTest.parse("passed=1\nstage=done\nrenderer=Mesa a=b\nframe_ms_p95=1.250\nnoise\n")
        assertTrue(r.passed);assertEquals("done",r.stage);assertEquals("Mesa a=b",r.renderer)
        assertEquals(1.25,r.double("frame_ms_p95"),1e-9);assertEquals(-1L,r.int("missing"))
    }
    @Test fun passRequiresReadbackEveryPresentWindowFramesAndMatchingPixels() {
        assertEquals(emptyList<String>(),SystemGpuSelfTest.problems(passing(180),180,good,null))
        assertTrue(SystemGpuSelfTest.problems(passing(179),180,good,null).isNotEmpty())
        assertTrue(SystemGpuSelfTest.problems(passing(180),180,null,null).isNotEmpty())
        assertTrue(SystemGpuSelfTest.problems(passing(180),180,good.take(3),null).isNotEmpty())
        assertTrue(SystemGpuSelfTest.problems(passing(180),180,List(4){ listOf(0,0,0,255) },null).isNotEmpty())
        assertTrue(SystemGpuSelfTest.problems(passing(180),180,good,"GPU channel closed").isNotEmpty())
        val headless=SystemGpuSelfTest.parse(passing(180).values.toMutableMap().apply { put("presented_to_window","0") }
            .entries.joinToString("\n") { "${it.key}=${it.value}" })
        assertTrue(SystemGpuSelfTest.problems(headless,180,good,null).any { it.contains("Surface") })
        val failed=SystemGpuSelfTest.parse("passed=0\nstage=shader\n")
        assertTrue(SystemGpuSelfTest.problems(failed,180,good,null).first().contains("shader"))
    }
    @Test fun pixelToleranceAcceptsDitherButNotAnotherColour() {
        assertTrue(SystemGpuSelfTest.pixelMatches(listOf(224,41,3,255)))
        assertFalse(SystemGpuSelfTest.pixelMatches(listOf(225,45,7,255)))
        assertFalse(SystemGpuSelfTest.pixelMatches(listOf(220,45,7)))
    }
    /** Actual C client (builtin crypto) -> EGP2 -> Kotlin endpoint -> JNI -> host ES3. */
    @Test fun nativeClientRunsTheFullGuestShapedScenarioAgainstTheRealHost() {
        assumeTrue(System.getProperty("ember.systemGpuNativeTest")=="true")
        SystemGpuEndpoint(secret,run,{SystemGpuNativeBackend()}).use { host ->
            var held=0
            val report=SystemGpuSelfTest.run(host.port,secret,run,24) { held++;true }
            assertTrue(report.values.toString(),report.passed)
            assertEquals(1,held)
            assertEquals(SystemGpuSelfTest.PANEL_BYTES.toLong(),report.int("readback_bytes"))
            assertEquals(24L,report.int("presents"));assertEquals(24L,report.int("frames"))
            // The JVM host has no window: frames are retained, never claimed as shown.
            assertEquals(0L,report.int("presented_to_window"))
            assertTrue(report.renderer.isNotBlank());assertEquals("not_verified",report.values["system_iOS_GPU"])
            assertTrue(report.double("frame_ms_p95")>0 && report.double("frame_ms_p95")<=report.double("frame_ms_max"))
            assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNull(host.failure)
        }
    }
    @Test fun refusedHoldFailsTheRunAndWrongCapabilityNeverAuthenticates() {
        assumeTrue(System.getProperty("ember.systemGpuNativeTest")=="true")
        SystemGpuEndpoint(secret,run,{SystemGpuNativeBackend()}).use { host ->
            val report=SystemGpuSelfTest.run(host.port,secret,run,2) { false }
            assertFalse(report.passed);assertEquals("hold",report.stage)
        }
        SystemGpuEndpoint(secret,run,{SystemGpuNativeBackend()}).use { host ->
            val wrong=secret.copyOf().also { it[0]=(it[0]+1).toByte() }
            val report=SystemGpuSelfTest.run(host.port,wrong,run,2,null)
            assertFalse(report.passed);assertEquals("auth",report.stage);assertFalse(host.authenticated)
        }
    }
}
