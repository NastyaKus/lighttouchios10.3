// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test
import org.junit.Assume.assumeNotNull

class SystemGpuEndpointTest {
    private val secret=ByteArray(32){it.toByte()};private val run=ByteArray(16){(it+7).toByte()}
    private class FakeBackend:SystemGpuEndpoint.Backend {
        override val capabilities=SystemGpuEndpoint.Capabilities(3,0,16384,"TEST BACKEND: no GPU execution")
        val contexts=mutableSetOf<Long>();var calls=0
        @Volatile var closed=false
        override fun create(context:Long,share:Long,width:Int,height:Int) { check(share==0L || share in contexts);check(contexts.add(context)) }
        override fun destroy(context:Long) { check(contexts.remove(context)) }
        override fun call(context:Long,command:ByteArray,upload:ByteArray?):ByteArray {
            check(context in contexts);calls++
            return if(upload==null)command else MessageDigest.getInstance("SHA-256").digest(upload)
        }
        override fun present(context:Long,surface:Long,serial:Long)=ByteArray(0)
        override fun close() { contexts.clear();closed=true }
    }
    private fun ints(vararg values:Int)=ByteBuffer.allocate(values.size*4).order(ByteOrder.LITTLE_ENDIAN).apply { values.forEach(::putInt) }.array()
    private fun long(value:Long)=ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array()
    private inner class Peer(port:Int):AutoCloseable {
        val socket=Socket("127.0.0.1",port).apply { soTimeout=3000;tcpNoDelay=true }
        private var sequence=0L
        init { SystemGpuWire.authenticateHost(socket.getInputStream(),socket.getOutputStream(),secret,run) }
        fun rpc(op:Int,context:Long=0,payload:ByteArray=byteArrayOf(),status:Int=0):ByteArray {
            val frame=SystemGpuWire.Frame(op,++sequence,context,payload)
            SystemGpuWire.write(socket.getOutputStream(),frame)
            val reply=SystemGpuWire.read(socket.getInputStream())
            assertEquals(op or SystemGpuWire.RESPONSE,reply.opcode);assertEquals(sequence,reply.sequence);assertEquals(context,reply.context)
            assertEquals(status,ByteBuffer.wrap(reply.payload).order(ByteOrder.LITTLE_ENDIAN).int)
            return reply.payload.copyOfRange(4,reply.payload.size)
        }
        fun hello(features:Int=0)=rpc(SystemGpuWire.HELLO,payload=ints(3,0,features,0))
        fun create(id:Long,share:Int=0)=rpc(SystemGpuWire.CONTEXT_CREATE,id,ints(828,1792,share))
        override fun close()=socket.close()
    }
    @Test fun persistentSessionTransfersLargeTextureAndCleansAllResourcesOnClose() {
        val backend=FakeBackend()
        SystemGpuEndpoint(secret,run,{backend}).use { host ->
            Peer(host.port).use { peer ->
                peer.hello();peer.create(1);peer.create(2,1)
                val data=ByteArray(2*1024*1024+91){(it*13).toByte()};val hash=MessageDigest.getInstance("SHA-256").digest(data)
                peer.rpc(SystemGpuWire.BLOB_BEGIN,1,long(7)+ints(data.size)+hash)
                var at=0
                while(at<data.size) {
                    val n=minOf(data.size-at,SystemGpuWire.MAX_PAYLOAD-12)
                    peer.rpc(SystemGpuWire.BLOB_CHUNK,1,long(7)+ints(at)+data.copyOfRange(at,at+n));at+=n
                }
                peer.rpc(SystemGpuWire.BLOB_COMMIT,1,long(7))
                assertArrayEquals(hash,peer.rpc(SystemGpuWire.CALL,1,long(7)+ints(42)))
                peer.rpc(SystemGpuWire.PRESENT,1,long(8)+long(1))
                peer.rpc(SystemGpuWire.CONTEXT_DESTROY,2);peer.rpc(SystemGpuWire.CLOSE)
            }
            assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNull(host.failure)
            assertEquals(1,backend.calls);assertTrue(backend.closed);assertTrue(backend.contexts.isEmpty())
        }
    }
    @Test fun rejectsCrossContextTextureReuseBeforeBackendExecutes() {
        val backend=FakeBackend()
        SystemGpuEndpoint(secret,run,{backend}).use { host ->
            Peer(host.port).use { peer ->
                peer.hello();peer.create(1);peer.create(2)
                val data=byteArrayOf(5);val hash=MessageDigest.getInstance("SHA-256").digest(data)
                peer.rpc(SystemGpuWire.BLOB_BEGIN,1,long(7)+ints(1)+hash)
                peer.rpc(SystemGpuWire.BLOB_CHUNK,1,long(7)+ints(0)+data)
                peer.rpc(SystemGpuWire.BLOB_COMMIT,1,long(7))
                peer.rpc(SystemGpuWire.CALL,2,long(7)+ints(42),status=1)
            }
            assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNotNull(host.failure)
            assertEquals(0,backend.calls);assertTrue(backend.closed)
        }
    }
    @Test fun idleDoesNotHaveDiagnosticTimeoutButPartialFrameDoes() {
        val backend=FakeBackend()
        SystemGpuEndpoint(secret,run,{backend},deadlineMs=100).use { host ->
            Peer(host.port).use { peer ->
                peer.hello();Thread.sleep(250);peer.create(1) // Idle exceeds the complete transaction deadline.
                peer.socket.getOutputStream().write('E'.code);peer.socket.getOutputStream().flush()
                assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNotNull(host.failure);assertTrue(backend.closed)
            }
        }
    }
    @Test fun cancellationInterruptsIdleSocketAndReleasesBackendOnItsThread() {
        val backend=FakeBackend();val host=SystemGpuEndpoint(secret,run,{backend})
        try {
            Peer(host.port).use { peer ->
                peer.hello();host.close();assertTrue(host.awaitClosed(3,TimeUnit.SECONDS))
                assertTrue(backend.closed);assertNull(host.failure)
            }
        } finally { host.close() }
    }
    @Test fun noRendererCanBeAdvertisedAsEs3WithoutActualEs3Capabilities() {
        val backend=object:SystemGpuEndpoint.Backend {
            override val capabilities=SystemGpuEndpoint.Capabilities(2,0,2048,"ES2")
            override fun create(context:Long,share:Long,width:Int,height:Int)=error("unexpected")
            override fun destroy(context:Long)=error("unexpected")
            override fun call(context:Long,command:ByteArray,upload:ByteArray?):ByteArray=error("unexpected")
            override fun present(context:Long,surface:Long,serial:Long):ByteArray=error("unexpected")
            override fun close() {}
        }
        SystemGpuEndpoint(secret,run,{backend}).use { host ->
            Peer(host.port).use { it.rpc(SystemGpuWire.HELLO,payload=ints(3,0,0,0),status=1) }
            assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNotNull(host.failure)
        }
    }
    @Test fun actualNativeCGuestAndKotlinHostAgreeOnAuthenticationFramesAndLargeUploads() {
        val executable=System.getProperty("ember.systemGpuPeer");assumeNotNull(executable)
        val backend=FakeBackend()
        SystemGpuEndpoint(secret,run,{backend}).use { host ->
            val process=ProcessBuilder(executable,host.port.toString()).redirectErrorStream(true).start()
            try {
                process.outputStream.use { it.write(secret+run) }
                assertTrue(process.waitFor(15,TimeUnit.SECONDS))
                val text=process.inputStream.bufferedReader().readText()
                assertEquals(text,0,process.exitValue());assertTrue(text,text.contains("EGP2 C/Kotlin PASS"))
                assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNull(host.failure)
                assertEquals(1,backend.calls);assertTrue(backend.closed)
            } finally { process.destroyForcibly() }
        }
    }
    @Test fun actualNativeExecutorAndCClientReturnACompleteGpuRenderedPanelThroughChunkedReadback() {
        val executable=System.getProperty("ember.systemGpuNativeClient")
        assumeNotNull(executable)
        org.junit.Assume.assumeTrue(System.getProperty("ember.systemGpuNativeTest")=="true")
        SystemGpuEndpoint(secret,run,{SystemGpuNativeBackend()}).use { host ->
            val process=ProcessBuilder(executable,host.port.toString()).redirectErrorStream(true).start()
            try {
                process.outputStream.use { it.write(secret+run) }
                assertTrue(process.waitFor(30,TimeUnit.SECONDS))
                val text=process.inputStream.bufferedReader().readText()
                assertEquals(text,0,process.exitValue());assertTrue(text,text.contains("EGP2 native backend PASS"))
                assertTrue(text,text.contains("readback_bytes=5935104"))
                assertTrue(text,text.contains("system_iOS_GPU=not_verified"))
                assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNull(host.failure)
            } finally { process.destroyForcibly() }
        }
    }
}
