// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.Closeable
import java.io.EOFException
import java.io.PushbackInputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Persistent transport owner for a single VM boot. This class does not create
 * a fake ES3 renderer: a real GPU backend must be supplied by the owner.
 * Backend calls (including close) all run on one worker, suitable for EGL.
 * Not connected to ordinary VM startup until the system frontend is complete. */
class SystemGpuEndpoint(secret:ByteArray,run:ByteArray,private val factory:()->Backend,
    private val deadlineMs:Int=15000):Closeable {
    data class Capabilities(val major:Int,val minor:Int,val maxTextureSize:Int,val renderer:String)
    interface Backend:Closeable {
        val capabilities:Capabilities
        fun create(context:Long,share:Long,width:Int,height:Int)
        fun destroy(context:Long)
        /** command includes its u32 operation followed by typed arguments.
         * The backend must reject unsupported operations; no silent no-ops. */
        fun call(context:Long,command:ByteArray,upload:ByteArray?):ByteArray
        fun present(context:Long,surface:Long,serial:Long):ByteArray
    }
    private val secret=secret.copyOf().also { require(it.size==32) }
    private val run=run.copyOf().also { require(it.size==16) }
    private val listener=ServerSocket().apply {
        require(deadlineMs in 100..60000)
        reuseAddress=false;bind(java.net.InetSocketAddress(InetAddress.getByName("127.0.0.1"),0),1)
    }
    val port:Int get()=listener.localPort
    @Volatile private var stopped=false
    @Volatile private var client:Socket?=null
    @Volatile var failure:Exception?=null;private set
    @Volatile var authenticated=false;private set
    private val done=CountDownLatch(1)
    private val deadlines=Executors.newSingleThreadScheduledExecutor { task ->
        Thread(task,"ember.system-gpu-deadline").apply { isDaemon=true }
    }
    private val worker=Thread(::serve,"ember.system-gpu").apply { start() }

    private fun serve() {
        try {
            while(!stopped) {
                val socket=listener.accept();client=socket
                socket.use {
                    if(stopped)return
                    socket.soTimeout=deadlineMs;socket.tcpNoDelay=true
                    try { SystemGpuWire.authenticateGuest(socket.getInputStream(),socket.getOutputStream(),secret,run) }
                    catch(_:Exception) { return@use } // A local unauthenticated connection owns no GL objects.
                    authenticated=true
                    timed(socket,"GPU backend initialization") { factory() }.use { backend -> dispatch(socket,backend) }
                }
                client=null
                // Never reconnect a new guest to stale contexts in the same run.
                if(authenticated)break
            }
        } catch(e:Throwable) {
            if(!stopped && failure==null)failure=if(e is Exception)e else IllegalStateException("GPU backend failed (${e.javaClass.simpleName})",e)
        }
        finally {
            stopped=true;runCatching { client?.close() };runCatching { listener.close() }
            deadlines.shutdownNow();secret.fill(0);run.fill(0);done.countDown()
        }
    }
    private fun le(bytes:ByteArray)=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
    private fun <T> timed(socket:Socket,operation:String,body:()->T):T {
        val active=AtomicBoolean(true)
        val timeout=deadlines.schedule({
            if(active.compareAndSet(true,false) && !stopped) {
                failure=SocketTimeoutException("$operation exceeded ${deadlineMs}ms")
                runCatching { socket.close() }
            }
        },deadlineMs.toLong(),TimeUnit.MILLISECONDS)
        try { return body() } finally { active.set(false);timeout.cancel(false) }
    }
    private fun u32(value:Int)=value.toLong() and 0xffffffffL
    private fun words(vararg values:Long)=ByteBuffer.allocate(values.size*4).order(ByteOrder.LITTLE_ENDIAN).apply {
        for(value in values)putInt(value.toInt())
    }.array()

    private fun dispatch(socket:Socket,backend:Backend) {
        val input=PushbackInputStream(socket.getInputStream(),1);val output=socket.getOutputStream();val sequence=SystemGpuWire.Sequence()
        val contexts=mutableSetOf<Long>();val blob=SystemGpuWire.Blob()
        var negotiated=false;var features=0;var uploading=0L;var uploadContext=0L
        var committed=0L;var committedContext=0L;var upload:ByteArray?=null;var lastSerial=0L
        var readback:ByteArray?=null;var readbackId=0L;var readbackContext=0L;var readbackOffset=0
        try {
            while(!stopped) {
                // Idle is valid for an interactive VM. Once a frame starts,
                // bound the complete read/execute/write transaction, not its age.
                socket.soTimeout=0
                val first=input.read();if(first<0)throw EOFException("GPU peer disconnected")
                input.unread(first);socket.soTimeout=deadlineMs
                timed(socket,"GPU transaction") {
                val frame=SystemGpuWire.read(input);sequence.accept(frame)
                require(frame.opcode and SystemGpuWire.RESPONSE==0)
                val p=le(frame.payload)
                var result=byteArrayOf();var closing=false
                try {
                    require(negotiated || frame.opcode==SystemGpuWire.HELLO) { "EGP2 HELLO required" }
                    require(readback==null || frame.opcode in setOf(SystemGpuWire.READBACK_CHUNK,SystemGpuWire.READBACK_RELEASE,SystemGpuWire.CLOSE)) {
                        "Pending EGP2 CPU readback must be consumed or the run closed"
                    }
                    when(frame.opcode) {
                        SystemGpuWire.HELLO -> {
                            require(!negotiated && frame.context==0L && frame.payload.size==16)
                            require(p.int==3 && p.int==0) { "Unsupported requested GPU profile" }
                            features=p.int
                            require(features and SystemGpuWire.FEATURE_READBACK.inv()==0 && p.int==0) { "Unsupported EGP2 feature" }
                            val cap=backend.capabilities
                            require(cap.major>=3 && cap.minor>=0 && cap.maxTextureSize in 2048..65536) { "Native ES3 capability required" }
                            val text=cap.renderer.toByteArray(Charsets.UTF_8)
                            require(text.size in 1..1024 && !text.contains(0.toByte()))
                            result=words(cap.major.toLong(),cap.minor.toLong(),cap.maxTextureSize.toLong(),text.size.toLong())+text
                            negotiated=true
                        }
                        SystemGpuWire.CONTEXT_CREATE -> {
                            require(frame.payload.size==12 && frame.context in 1..0xffffffffL && frame.context !in contexts && contexts.size<8)
                            val width=p.int;val height=p.int;val share=u32(p.int)
                            require(width in 1..8192 && height in 1..8192 && (share==0L || share in contexts))
                            backend.create(frame.context,share,width,height);contexts+=frame.context
                        }
                        SystemGpuWire.CONTEXT_DESTROY -> {
                            require(frame.payload.isEmpty() && frame.context in contexts)
                            require(uploadContext!=frame.context && committedContext!=frame.context)
                            backend.destroy(frame.context);contexts-=frame.context
                        }
                        SystemGpuWire.BLOB_BEGIN -> {
                            require(frame.context in contexts && frame.payload.size==44 && uploading==0L && committed==0L)
                            val id=p.long;val size=p.int;val hash=ByteArray(32).also { p.get(it) }
                            blob.begin(id,size,hash);uploading=id;uploadContext=frame.context
                        }
                        SystemGpuWire.BLOB_CHUNK -> {
                            require(frame.context==uploadContext && frame.payload.size>12 && uploading!=0L)
                            val id=p.long;val offset=p.int
                            blob.chunk(id,offset,frame.payload.copyOfRange(12,frame.payload.size))
                        }
                        SystemGpuWire.BLOB_COMMIT -> {
                            require(frame.context==uploadContext && frame.payload.size==8 && uploading!=0L)
                            val id=p.long;upload=blob.commit(id);committed=id;committedContext=uploadContext
                            uploading=0;uploadContext=0
                        }
                        SystemGpuWire.CALL,SystemGpuWire.READBACK_CALL -> {
                            require(frame.context in contexts && frame.payload.size>=12 && uploading==0L)
                            require(frame.opcode!=SystemGpuWire.READBACK_CALL || features and SystemGpuWire.FEATURE_READBACK!=0) { "CPU readback feature was not negotiated" }
                            val id=p.long
                            require(id==0L || (id==committed && frame.context==committedContext)) { "Unknown/cross-context upload" }
                            result=backend.call(frame.context,frame.payload.copyOfRange(8,frame.payload.size),if(id==0L)null else upload)
                            if(id!=0L){upload=null;committed=0;committedContext=0}
                            if(frame.opcode==SystemGpuWire.READBACK_CALL) {
                                require(result.size<=SystemGpuWire.MAX_BLOB && readbackId<Long.MAX_VALUE) { "CPU readback bounds" }
                                readbackId++;readbackContext=frame.context;readbackOffset=0;readback=result
                                val hash=MessageDigest.getInstance("SHA-256").digest(result)
                                result=ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
                                    .putLong(readbackId).putInt(result.size).put(hash).array()
                            }
                        }
                        SystemGpuWire.READBACK_CHUNK -> {
                            val data=readback
                            require(features and SystemGpuWire.FEATURE_READBACK!=0 && data!=null && frame.context==readbackContext && frame.payload.size==16)
                            val id=p.long;val offset=p.int;val count=p.int
                            require(id==readbackId && offset==readbackOffset && count in 1..SystemGpuWire.MAX_PAYLOAD-16 && count<=data.size-offset) { "Invalid CPU readback slice" }
                            result=ByteBuffer.allocate(12+count).order(ByteOrder.LITTLE_ENDIAN).putLong(id).putInt(offset)
                                .put(data,offset,count).array();readbackOffset+=count
                        }
                        SystemGpuWire.READBACK_RELEASE -> {
                            val data=readback
                            require(features and SystemGpuWire.FEATURE_READBACK!=0 && data!=null && frame.context==readbackContext && frame.payload.size==8)
                            require(p.long==readbackId && readbackOffset==data.size) { "Incomplete CPU readback" }
                            readback=null;readbackContext=0;readbackOffset=0
                        }
                        SystemGpuWire.PRESENT -> {
                            require(frame.context in contexts && frame.payload.size==16 && uploading==0L)
                            val surface=p.long;val serial=p.long
                            require(surface>0 && serial>lastSerial) { "Invalid presentation identity" }
                            result=backend.present(frame.context,surface,serial);lastSerial=serial
                        }
                        SystemGpuWire.CLOSE -> {
                            require(frame.context==0L && frame.payload.isEmpty());closing=true
                        }
                        else -> error("Unsupported EGP2 operation")
                    }
                    require(result.size<=SystemGpuWire.MAX_PAYLOAD-4) { "Oversized GPU result" }
                } catch(e:Exception) {
                    // Return an explicit failed response and tear down all owned resources.
                    runCatching { SystemGpuWire.write(output,frame.copy(opcode=frame.opcode or SystemGpuWire.RESPONSE,payload=words(1))) }
                    throw e
                }
                SystemGpuWire.write(output,frame.copy(opcode=frame.opcode or SystemGpuWire.RESPONSE,payload=words(0)+result))
                !closing
                }.also { keep -> if(!keep)return }
            }
        } finally { blob.abort();upload=null;readback=null }
    }
    override fun close() {
        stopped=true;runCatching { listener.close() };runCatching { client?.close() }
        // Closing the socket also interrupts blocked writes; an overall five-minute
        // diagnostic timeout would incorrectly kill a normal interactive session.
    }
    fun awaitClosed(timeout:Long,unit:TimeUnit)=done.await(timeout,unit)
}
