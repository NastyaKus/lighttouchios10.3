// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import org.junit.Assert.*
import org.junit.Test

class GpuLabEvidenceTest {
    private val host=GpuLabHostResult(true,"test","Adreno test fixture","GLES test fixture","",119,7,16391,194,18,4,5)
    private val console="""
        *** EGPU-GLES2: real compiler rejected bad shader (55 log bytes)
        *** EGPU-GLES2: batched ten GL calls into one request
        *** EGPU-EAGL: PASS API=2 current/detach/reattach; TLS ownership verified; unsupported API=3 and drawable return failure
        *** EGPU-GLES2: PASS checks=40 packets=118 shaders/textures/buffers/blend/scissor/packing/TLS
    """.trimIndent()
    private fun rejects(action:()->Unit){try { action();fail("Expected rejection") } catch(_:IllegalArgumentException){} catch(_:IllegalStateException){} }
    @Test fun acceptsOnlyCompleteMutualEvidence() { assertEquals(40,GpuLabEvidence.verify(console,host)) }
    @Test fun guestPassAloneAndHostPassAloneCannotProveGraphics() {
        rejects { GpuLabEvidence.verify(console,host.copy(success=false,error="EGL failed")) }
        rejects { GpuLabEvidence.verify("",host) }
        rejects { GpuLabEvidence.verify(console,host.copy(commands=118)) }
        rejects { GpuLabEvidence.verify(console,host.copy(shaderCompiles=0)) }
        rejects { GpuLabEvidence.verify(console,host.copy(pixels=2)) }
        rejects { GpuLabEvidence.verify(console,host.copy(apiDraws=0)) }
    }
    @Test fun panicOrMissingEaglCompilerAndBatchTestsCannotPass() {
        rejects { GpuLabEvidence.verify(console+"\npanic(cpu 0 caller test)",host) }
        rejects { GpuLabEvidence.verify(console+"\n*** EGPU-GLES2: FAIL",host) }
        rejects { GpuLabEvidence.verify(console.replace("TLS ownership verified;",""),host) }
        rejects { GpuLabEvidence.verify(console.replace("real compiler rejected bad shader",""),host) }
        rejects { GpuLabEvidence.verify(console.replace("batched ten GL calls into one request",""),host) }
    }
    @Test fun jniArrayHasStrictFieldCountAndUnsignedCounters() {
        val v=arrayOf("1","test","Adreno test fixture","GLES test fixture","","119","7","16391","194","18","4","5")
        assertEquals(host,GpuLabEvidence.decodeNative(v))
        rejects { GpuLabEvidence.decodeNative(v.copyOfRange(0,11)) }
        rejects { GpuLabEvidence.decodeNative(v.copyOf().also { it[5]="-1" }) }
        rejects { GpuLabEvidence.decodeNative(v.copyOf().also { it[5]="18446744073709551615" }) }
        rejects { GpuLabEvidence.decodeNative(v.copyOf().also { it[0]="true" }) }
    }
}
