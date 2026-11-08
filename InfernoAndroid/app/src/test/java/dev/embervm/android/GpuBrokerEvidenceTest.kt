// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android
import org.junit.Assert.*
import org.junit.Test

class GpuBrokerEvidenceTest {
    private val console="""
        *** EGPU-BROKER-CLIENT: uid=501 gid=501 raw_read_denied=13 raw_write_denied=1
        *** EGPU-BROKER: authenticated uid=501 gid=501
        *** EGPU-BROKER: PASS uid=501 gid=501 forwarded=316 replayed=0 child_exit=0 cleanup=1
    """.trimIndent()
    private fun rejects(text:String,commands:Long=316) { assertTrue(runCatching { GpuBrokerEvidence.verify(text,commands) }.isFailure) }
    @Test fun acceptsCompleteMobileProofWithMatchingHostCounter() {
        assertTrue(GpuBrokerEvidence.finished(console));GpuBrokerEvidence.verify(console,316)
        GpuBrokerEvidence.verify(console.replace("\n","\r\n"),316)
        GpuBrokerEvidence.verify(console.replace("\n","\r\r\n"),316)
    }
    @Test fun refusesWrongIdentityOrOpenPermissionAndMissingProof() {
        for((from,to) in listOf("uid=501" to "uid=0","gid=501" to "gid=0","raw_read_denied=13" to "raw_read_denied=0",
            "raw_write_denied=1" to "raw_write_denied=2","authenticated" to "unauthenticated",
            "child_exit=0" to "child_exit=1","cleanup=1" to "cleanup=0","replayed=0" to "replayed=1"))rejects(console.replace(from,to))
        rejects(console.substringBefore("*** EGPU-BROKER: PASS"));rejects(console.substringAfter("*** EGPU-BROKER: PASS"))
    }
    @Test fun gpuCompletionOrChildSuccessAloneIsInsufficient() {
        rejects("*** EGPU-GLES2: PASS checks=40 packets=315");rejects(console,315);rejects(console,0)
        rejects(console+"\n*** EGPU-BROKER: FAIL cleanup")
        rejects(console+"\n*** EGPU-BROKER-CLIENT: FAIL raw open")
        rejects(console.replace("cleanup=1","cleanup=10"))
    }
}
