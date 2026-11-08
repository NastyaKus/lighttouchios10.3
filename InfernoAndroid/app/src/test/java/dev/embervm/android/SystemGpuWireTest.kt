// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.ByteArrayInputStream
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test

class SystemGpuWireTest {
    @Test fun frameRoundtripAndLittleEndianKnownLayout() {
        val frame=SystemGpuWire.Frame(SystemGpuWire.CALL,0x12345678,0x90abcdef,byteArrayOf(1,2,3))
        val encoded=SystemGpuWire.encode(frame)
        assertArrayEquals(byteArrayOf(0x78,0x56,0x34,0x12),encoded.copyOfRange(8,12))
        val parsed=SystemGpuWire.decode(encoded);assertEquals(frame.opcode,parsed.opcode);assertEquals(frame.context,parsed.context);assertEquals(frame.sequence,parsed.sequence);assertArrayEquals(frame.payload,parsed.payload)
        val slow=object:ByteArrayInputStream(encoded){override fun read(b:ByteArray,off:Int,len:Int)=super.read(b,off,minOf(len,1))}
        assertArrayEquals(frame.payload,SystemGpuWire.read(slow).payload)
    }
    @Test fun rejectsCorruptionTruncationOversizeAndReplay() {
        val bytes=SystemGpuWire.encode(SystemGpuWire.Frame(SystemGpuWire.CALL,1,1,byteArrayOf(1,2,3)))
        for(i in bytes.indices){val broken=bytes.copyOf();broken[i]=(broken[i].toInt() xor 1).toByte();assertTrue("byte $i",runCatching { SystemGpuWire.decode(broken) }.isFailure)}
        for(i in bytes.indices)assertTrue(runCatching { SystemGpuWire.read(ByteArrayInputStream(bytes.copyOf(i))) }.isFailure)
        assertTrue(runCatching { SystemGpuWire.encode(SystemGpuWire.Frame(SystemGpuWire.CALL,1,1,ByteArray(SystemGpuWire.MAX_PAYLOAD+1))) }.isFailure)
        val order=SystemGpuWire.Sequence();val frame=SystemGpuWire.decode(bytes);order.accept(frame)
        assertTrue(runCatching { order.accept(frame) }.isFailure);assertTrue(runCatching { order.accept(frame.copy(sequence=3)) }.isFailure)
    }
    private fun authenticate(wrongKey:Boolean=false,wrongRun:Boolean=false):Pair<Boolean,Boolean> {
        val secret=ByteArray(32){it.toByte()};val run=ByteArray(16){(it+10).toByte()};val pool=Executors.newSingleThreadExecutor()
        ServerSocket(0,1,java.net.InetAddress.getLoopbackAddress()).use { listener ->
            val server=pool.submit<Boolean> { listener.accept().use { socket -> socket.soTimeout=2000
                runCatching { SystemGpuWire.authenticateGuest(socket.getInputStream(),socket.getOutputStream(),secret,run) }.isSuccess } }
            try {
                val ok=Socket(java.net.InetAddress.getLoopbackAddress(),listener.localPort).use { socket -> socket.soTimeout=2000
                    runCatching { SystemGpuWire.authenticateHost(socket.getInputStream(),socket.getOutputStream(),
                        secret.copyOf().also { if(wrongKey)it[0]=99 },run.copyOf().also { if(wrongRun)it[0]=99 }) }.isSuccess }
                return ok to server.get(4,TimeUnit.SECONDS)
            } finally { pool.shutdownNow();pool.awaitTermination(4,TimeUnit.SECONDS) }
        }
    }
    @Test fun actualSocketMutualAuthenticationRefusesWrongSecretOrRun() {
        assertEquals(true to true,authenticate());assertEquals(false to false,authenticate(wrongKey=true));assertEquals(false to false,authenticate(wrongRun=true))
    }
    @Test fun largeUploadIsPublishedOnlyAfterContiguousVerifiedCompletion() {
        val content=ByteArray(2*1024*1024+17){(it*31).toByte()};val hash=MessageDigest.getInstance("SHA-256").digest(content)
        val blob=SystemGpuWire.Blob();blob.begin(1,content.size,hash)
        var at=0
        while(at<content.size){val end=minOf(content.size,at+SystemGpuWire.MAX_PAYLOAD-12);blob.chunk(1,at,content.copyOfRange(at,end));at=end}
        assertArrayEquals(content,blob.commit(1))
        blob.begin(2,10,ByteArray(32));assertTrue(runCatching { blob.chunk(2,1,byteArrayOf(1)) }.isFailure)
        assertTrue(runCatching { blob.commit(2) }.isFailure)
        blob.begin(3,1,ByteArray(32));blob.chunk(3,0,byteArrayOf(1));assertTrue(runCatching { blob.commit(3) }.isFailure)
        blob.begin(4,1,MessageDigest.getInstance("SHA-256").digest(byteArrayOf(1)));assertTrue(runCatching { blob.begin(5,1,ByteArray(32)) }.isFailure)
        assertTrue(runCatching { blob.commit(4) }.isFailure)
    }
}
