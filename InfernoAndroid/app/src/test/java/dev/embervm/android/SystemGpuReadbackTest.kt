// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test

/** Protocol/lifetime faults, not synthetic proof of rendering performance. */
class SystemGpuReadbackTest {
    private val secret=ByteArray(32){(it*7).toByte()}
    private val run=ByteArray(16){(it+90).toByte()}
    private fun ints(vararg v:Int)=ByteBuffer.allocate(v.size*4).order(ByteOrder.LITTLE_ENDIAN).apply { v.forEach(::putInt) }.array()
    private fun long(v:Long)=ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(v).array()
    private class Backend(private val data:ByteArray):SystemGpuEndpoint.Backend {
        @Volatile var closed=false
        override val capabilities=SystemGpuEndpoint.Capabilities(3,0,4096,"TEST protocol backend")
        override fun create(context:Long,share:Long,width:Int,height:Int) {}
        override fun destroy(context:Long) {}
        override fun call(context:Long,command:ByteArray,upload:ByteArray?)=data
        override fun present(context:Long,surface:Long,serial:Long)=byteArrayOf()
        override fun close() {closed=true}
    }
    private inner class Peer(port:Int):AutoCloseable {
        val socket=Socket("127.0.0.1",port).apply { soTimeout=3000;tcpNoDelay=true }
        private var seq=0L
        init {SystemGpuWire.authenticateHost(socket.getInputStream(),socket.getOutputStream(),secret,run)}
        fun send(op:Int,ctx:Long=0,p:ByteArray=byteArrayOf()) {
            SystemGpuWire.write(socket.getOutputStream(),SystemGpuWire.Frame(op,++seq,ctx,p))
        }
        fun rpc(op:Int,ctx:Long=0,p:ByteArray=byteArrayOf(),status:Int=0):ByteArray {
            send(op,ctx,p);val r=SystemGpuWire.read(socket.getInputStream())
            assertEquals(seq,r.sequence);assertEquals(ctx,r.context);assertEquals(op or SystemGpuWire.RESPONSE,r.opcode)
            assertEquals(status,ByteBuffer.wrap(r.payload).order(ByteOrder.LITTLE_ENDIAN).int)
            return r.payload.copyOfRange(4,r.payload.size)
        }
        fun setup(features:Int=1){rpc(SystemGpuWire.HELLO,p=ints(3,0,features,0));rpc(SystemGpuWire.CONTEXT_CREATE,1,ints(1,1,0));rpc(SystemGpuWire.CONTEXT_CREATE,2,ints(1,1,0))}
        override fun close()=socket.close()
    }
    @Test fun fullReadbackRequiresContiguousChunksHashAndReleaseBeforeOtherCommands() {
        val data=ByteArray(2*1024*1024+17){(it*13).toByte()};val backend=Backend(data)
        SystemGpuEndpoint(secret,run,{backend}).use { host ->
            Peer(host.port).use { peer ->
                peer.setup()
                val descriptor=ByteBuffer.wrap(peer.rpc(SystemGpuWire.READBACK_CALL,1,long(0)+ints(1))).order(ByteOrder.LITTLE_ENDIAN)
                val id=descriptor.long;assertTrue(id>0);assertEquals(data.size,descriptor.int)
                val hash=ByteArray(32).also { descriptor.get(it) };assertArrayEquals(MessageDigest.getInstance("SHA-256").digest(data),hash)
                val collected=ByteArray(data.size);var at=0
                while(at<data.size) {
                    val n=minOf(data.size-at,SystemGpuWire.MAX_PAYLOAD-16)
                    val body=ByteBuffer.wrap(peer.rpc(SystemGpuWire.READBACK_CHUNK,1,long(id)+ints(at,n))).order(ByteOrder.LITTLE_ENDIAN)
                    assertEquals(id,body.long);assertEquals(at,body.int);body.get(collected,at,n);assertFalse(body.hasRemaining());at+=n
                }
                assertArrayEquals(data,collected)
                peer.rpc(SystemGpuWire.READBACK_RELEASE,1,long(id));peer.rpc(SystemGpuWire.CONTEXT_DESTROY,1);peer.rpc(SystemGpuWire.CLOSE)
            }
            assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNull(host.failure);assertTrue(backend.closed)
        }
    }
    @Test fun unnegotiatedReadbackAndWrongContextOffsetOrPrematureReleaseAreRejected() {
        for(mode in 0..5) {
            val backend=Backend(ByteArray(100))
            SystemGpuEndpoint(secret,run,{backend}).use { host ->
                Peer(host.port).use { peer ->
                    peer.setup(if(mode==0)0 else 1)
                    if(mode==0)peer.rpc(SystemGpuWire.READBACK_CALL,1,long(0)+ints(1),status=1)
                    else {
                        val id=ByteBuffer.wrap(peer.rpc(SystemGpuWire.READBACK_CALL,1,long(0)+ints(1))).order(ByteOrder.LITTLE_ENDIAN).long
                        when(mode) {
                            1 -> peer.rpc(SystemGpuWire.READBACK_CHUNK,2,long(id)+ints(0,1),status=1)
                            2 -> peer.rpc(SystemGpuWire.READBACK_CHUNK,1,long(id)+ints(1,1),status=1)
                            3 -> peer.rpc(SystemGpuWire.READBACK_CHUNK,1,long(id)+ints(0,101),status=1)
                            4 -> peer.rpc(SystemGpuWire.READBACK_RELEASE,1,long(id),status=1)
                            5 -> peer.rpc(SystemGpuWire.CALL,1,long(0)+ints(1),status=1)
                        }
                    }
                }
                assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertNotNull(host.failure);assertTrue(backend.closed)
            }
        }
    }
    @Test fun transactionWatchdogBecomesVisibleEvenWhenBackendDoesNotReturn() {
        val entered=CountDownLatch(1);val release=CountDownLatch(1);val record=Backend(byteArrayOf())
        val backend=object:SystemGpuEndpoint.Backend by record {
            override fun call(context:Long,command:ByteArray,upload:ByteArray?):ByteArray {
                entered.countDown();check(release.await(3,TimeUnit.SECONDS));return byteArrayOf()
            }
        }
        SystemGpuEndpoint(secret,run,{backend},deadlineMs=100).use { host ->
            try {
                Peer(host.port).use { peer ->
                    peer.setup();peer.send(SystemGpuWire.CALL,1,long(0)+ints(1));assertTrue(entered.await(1,TimeUnit.SECONDS))
                    val until=System.nanoTime()+TimeUnit.SECONDS.toNanos(2)
                    while(host.failure==null && System.nanoTime()<until)Thread.sleep(10)
                    assertNotNull("The service must see timeout while native GL is still blocked",host.failure)
                    assertTrue(host.failure!!.message!!.contains("GPU transaction"))
                }
            } finally {release.countDown()}
            assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertTrue(record.closed)
        }
    }
    @Test fun initializationHasAWatchdogBeforeFirstGlContextExists() {
        val entered=CountDownLatch(1);val release=CountDownLatch(1);val backend=Backend(byteArrayOf())
        SystemGpuEndpoint(secret,run,{entered.countDown();check(release.await(3,TimeUnit.SECONDS));backend},deadlineMs=100).use { host ->
            try {
                Peer(host.port).use {
                    assertTrue(entered.await(1,TimeUnit.SECONDS));val until=System.nanoTime()+TimeUnit.SECONDS.toNanos(2)
                    while(host.failure==null && System.nanoTime()<until)Thread.sleep(10)
                    assertNotNull(host.failure);assertTrue(host.failure!!.message!!.contains("initialization"))
                }
            } finally {release.countDown()}
            assertTrue(host.awaitClosed(3,TimeUnit.SECONDS));assertTrue(backend.closed)
        }
    }
}
