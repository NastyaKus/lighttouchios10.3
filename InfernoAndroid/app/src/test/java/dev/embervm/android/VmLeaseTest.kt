// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.File
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class VmLeaseTest {
    @get:Rule val tmp=TemporaryFolder()
    private fun command(root:File,vararg extra:String):List<String> {
        val classes=listOf(VmLease::class.java,VmLeaseFixture::class.java,kotlin.Unit::class.java)
        val cp=classes.map { File(requireNotNull(it.protectionDomain).codeSource.location.toURI()).path }.distinct().joinToString(File.pathSeparator)
        return listOf(File(System.getProperty("java.home"),"bin/java").path,"-cp",cp,
            VmLeaseFixture::class.java.name,root.path)+extra
    }
    @Test fun excludesCompetingOwnersAndAllowsReuseAfterClose() {
        val root=tmp.newFolder();val first=VmLease.acquire(root)
        try { assertNull(VmLease.tryAcquire(root)) } finally { first.close();first.close() }
        VmLease.acquire(root).close()
        assertTrue(File(root,"vm-owner.lock").isFile) // Existence is not ownership.
    }
    @Test fun kernelReleasesLockAfterOwningProcessIsKilled() {
        val root=tmp.newFolder()
        val child=ProcessBuilder(command(root)).redirectErrorStream(true).start()
        try {
            val until=System.nanoTime()+TimeUnit.SECONDS.toNanos(8)
            while(child.isAlive && !File(root,"ready").exists() && System.nanoTime()<until)Thread.sleep(10)
            assertTrue("Lock holder failed to start",File(root,"ready").isFile)
            assertNull(VmLease.tryAcquire(root))
            child.destroyForcibly();assertTrue(child.waitFor(5,TimeUnit.SECONDS))
            VmLease.acquire(root).close()
        } finally { child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS) }
    }
    @Test fun failedLocalAttemptMustNotDropFirstOwnersKernelLock() {
        val root=tmp.newFolder()
        VmLease.acquire(root).use {
            assertNull(VmLease.tryAcquire(root))
            val rival=ProcessBuilder(command(root,"try")).redirectErrorStream(true).start()
            try {
                assertTrue(rival.waitFor(8,TimeUnit.SECONDS))
                assertEquals(0,rival.exitValue())
                assertEquals("BLOCKED",rival.inputStream.bufferedReader().readText().trim())
            } finally { rival.destroyForcibly();rival.waitFor(5,TimeUnit.SECONDS) }
        }
    }
}
object VmLeaseFixture {
    @JvmStatic fun main(args:Array<String>) {
        if(args.size>1 && args[1]=="try") {
            val lease=VmLease.tryAcquire(File(args[0]));println(if(lease==null)"BLOCKED" else "ACQUIRED");lease?.close();return
        }
        VmLease.acquire(File(args[0])).use { File(args[0],"ready").writeText("ready");Thread.sleep(60_000) }
    }
}
