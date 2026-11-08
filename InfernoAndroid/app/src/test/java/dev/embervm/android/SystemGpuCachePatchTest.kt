// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.nio.file.Files
import java.io.RandomAccessFile
import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Test

class SystemGpuCachePatchTest {
    private fun hash(bytes:ByteArray)=MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
    private val first=byteArrayOf(1,2,3,4);private val second=byteArrayOf(5,6,7,8);private val keep=byteArrayOf(9,10,11,12)
    private val sites=listOf(
        SystemGpuCachePlan.Site(64,"aaaaaaaa",hash(first),true),
        SystemGpuCachePlan.Site(68,"bbbbbbbb",hash(second),true),
        SystemGpuCachePlan.Site(128,"cccccccc",hash(keep),false))
    private val backup="40: 1 2 3 4 5 6 7 8\n80: 9 a b c\n".toByteArray()
    @Test fun acceptsGroupedUpstreamOffsetsButChecksEveryOriginalByte() {
        val parsed=SystemGpuCachePatch.originals(backup,sites)
        assertArrayEquals(first,parsed.getValue(64));assertArrayEquals(second,parsed.getValue(68))
        for(text in listOf("40: 1 2", "40: 1 2 3 4 5 6 7 8 9", "40: 101", "ffffffffffffffff: 1", "40: 1 40: 1", "50: 1",backup.toString(Charsets.US_ASCII).replace("1 2","2 1"))) {
            assertTrue(text,runCatching { SystemGpuCachePatch.originals(text.toByteArray(),sites) }.isFailure)
        }
    }
    @Test fun distinguishesPartialRendererRestoreAndRefusesUnrelatedChangesWithoutWriting() {
        val path=Files.createTempFile("cache-patch-",".synthetic").toFile()
        try { RandomAccessFile(path,"rw").use { file ->
            file.setLength(256);file.seek(64);file.write(ByteArray(4){0xaa.toByte()});file.write(ByteArray(4){0xbb.toByte()})
            file.seek(128);file.write(ByteArray(4){0xcc.toByte()})
            val original=SystemGpuCachePatch.originals(backup,sites);val before=path.readBytes()
            assertEquals(SystemGpuCachePatch.State.SOFTWARE,SystemGpuCachePatch.inspect(file,sites,original));assertArrayEquals(before,path.readBytes())
            file.seek(64);file.write(first);assertEquals(SystemGpuCachePatch.State.PARTIAL,SystemGpuCachePatch.inspect(file,sites,original))
            file.seek(68);file.write(second);assertEquals(SystemGpuCachePatch.State.MAIN_EAGL_RESTORED,SystemGpuCachePatch.inspect(file,sites,original))
            file.seek(128);file.write(keep);assertTrue(runCatching { SystemGpuCachePatch.inspect(file,sites,original) }.isFailure)
        } } finally { path.delete() }
    }
    @Test fun productionPlanRestoresOnlyMainRendererAndRejectsWrongCache() {
        assertEquals(45,SystemGpuCachePlan.sites.size)
        assertEquals(listOf(0x8158bdcL,0x8158be0L),SystemGpuCachePlan.sites.filter { it.restore }.map { it.offset })
        val path=Files.createTempFile("wrong-cache-",".synthetic").toFile()
        try { RandomAccessFile(path,"rw").use { file ->
            file.setLength(1024);val before=path.readBytes()
            assertTrue(runCatching { SystemGpuCachePatch.prepare(file,backup) }.isFailure);assertArrayEquals(before,path.readBytes())
        } } finally { path.delete() }
    }
}
