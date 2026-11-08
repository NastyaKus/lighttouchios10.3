// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.File
import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermission
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SystemGpuChannelTest {
    @get:Rule val temp=TemporaryFolder()
    private val run=ByteArray(16){(it+1).toByte()};private val secret=ByteArray(32){(0x80+it).toByte()}
    private val descriptor=SystemGpuChannel.Descriptor(40123,run,secret,1_762_600_000_000L)

    @Test fun blockRoundTripsWithFixedLayout() {
        val block=SystemGpuChannel.encode(descriptor)
        assertEquals(4096,block.size)
        assertEquals("EGC1",String(block,0,4,Charsets.US_ASCII))
        assertEquals(1,block[4].toInt());assertEquals(0xbb,block[8].toInt() and 255);assertEquals(0x9c,block[9].toInt() and 255)
        assertArrayEquals(run,block.copyOfRange(16,32));assertArrayEquals(secret,block.copyOfRange(32,64))
        val back=SystemGpuChannel.decode(block)
        assertEquals(40123,back.port);assertEquals(descriptor.bootTime,back.bootTime)
        assertArrayEquals(run,back.run);assertArrayEquals(secret,back.secret)
    }
    @Test fun everySingleByteCorruptionIsRejected() {
        val block=SystemGpuChannel.encode(descriptor)
        for(i in block.indices) {
            val bad=block.copyOf().also { it[i]=(it[i].toInt() xor 1).toByte() }
            assertThrows(IllegalArgumentException::class.java) { SystemGpuChannel.decode(bad) }
        }
        assertThrows(IllegalArgumentException::class.java) { SystemGpuChannel.decode(block.copyOf(4095)) }
    }
    @Test fun descriptorRejectsBadPortAndSizes() {
        assertThrows(IllegalArgumentException::class.java) { SystemGpuChannel.Descriptor(0,run,secret,0) }
        assertThrows(IllegalArgumentException::class.java) { SystemGpuChannel.Descriptor(65536,run,secret,0) }
        assertThrows(IllegalArgumentException::class.java) { SystemGpuChannel.Descriptor(1,run.copyOf(15),secret,0) }
        assertThrows(IllegalArgumentException::class.java) { SystemGpuChannel.Descriptor(1,run,secret.copyOf(31),0) }
    }
    @Test fun writtenOwnerOnlyAtomicallyAndWipedAfterTheBoot() {
        val dir=temp.newFolder("private")
        SystemGpuChannel.write(dir,descriptor)
        val file=SystemGpuChannel.file(dir)
        assertTrue(file.isFile);assertFalse(File(file.path+".part").exists())
        assertEquals(setOf(PosixFilePermission.OWNER_READ,PosixFilePermission.OWNER_WRITE),Files.getPosixFilePermissions(file.toPath()))
        assertEquals(40123,SystemGpuChannel.decode(file.readBytes()).port)
        SystemGpuChannel.write(dir,descriptor.copy(port=40124))
        assertEquals(40124,SystemGpuChannel.decode(file.readBytes()).port)
        SystemGpuChannel.wipe(dir);assertFalse(file.exists())
        SystemGpuChannel.wipe(dir) // idempotent
    }
    @Test fun latchTripsWithReasonAndResets() {
        val latch=SystemGpuLatch(File(temp.root,"system-gpu/latch.txt"))
        assertFalse(latch.tripped)
        latch.trip("System GPU process disconnected")
        assertTrue(latch.tripped);assertEquals("System GPU process disconnected",latch.reason)
        latch.trip("x".repeat(2000));assertEquals(512,latch.reason.length)
        latch.reset();assertFalse(latch.tripped)
    }
    @Test fun experimentalHostIsOffByDefault() { assertFalse(VmSettings().systemGpuHost) }
}
