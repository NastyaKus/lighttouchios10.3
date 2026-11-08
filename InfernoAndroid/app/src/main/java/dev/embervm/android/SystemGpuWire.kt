// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.zip.CRC32
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** EGP2 transport. No GL execution or acceleration is implied by this layer.
 * Socket owners must set read/write deadlines and close on any protocol error. */
object SystemGpuWire {
    const val HEADER=32
    const val MAX_PAYLOAD=65536
    const val MAX_BLOB=64*1024*1024
    const val RESPONSE=0x8000
    const val HELLO=1
    const val CONTEXT_CREATE=2
    const val CONTEXT_DESTROY=3
    const val CALL=4
    const val PRESENT=5
    const val BLOB_BEGIN=6
    const val BLOB_CHUNK=7
    const val BLOB_COMMIT=8
    const val CLOSE=9
    const val READBACK_CALL=10
    const val READBACK_CHUNK=11
    const val READBACK_RELEASE=12
    const val FEATURE_READBACK=1
    data class Frame(val opcode:Int,val sequence:Long,val context:Long,val payload:ByteArray)
    private val authMagic="EGR2AUTH".toByteArray(Charsets.US_ASCII)
    private fun little(data:ByteArray)=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    private fun unsigned(i:Int)=i.toLong() and 0xffffffffL
    private fun validate(opcode:Int,sequence:Long,context:Long,size:Int) {
        require(opcode and RESPONSE.inv() in HELLO..READBACK_RELEASE && opcode in 0..65535 && sequence in 1..0xffffffffL &&
            context in 0..0xffffffffL && size in 0..MAX_PAYLOAD) { "Invalid EGP2 fields" }
    }
    private fun crc(header:ByteArray,payload:ByteArray)=CRC32().apply { update(header,0,24);update(payload) }.value.toInt()
    fun encode(frame:Frame):ByteArray {
        validate(frame.opcode,frame.sequence,frame.context,frame.payload.size)
        val bytes=ByteArray(HEADER+frame.payload.size);val b=little(bytes)
        b.put("EGP2".toByteArray(Charsets.US_ASCII));b.putShort(2);b.putShort(frame.opcode.toShort())
        b.putInt(frame.sequence.toInt());b.putInt(frame.context.toInt());b.putInt(frame.payload.size);b.putInt(0)
        b.putInt(crc(bytes,frame.payload));b.putInt(0);frame.payload.copyInto(bytes,HEADER)
        return bytes
    }
    fun decode(bytes:ByteArray):Frame {
        require(bytes.size>=HEADER);val header=bytes.copyOfRange(0,HEADER);val b=little(header)
        require(String(header,0,4,Charsets.US_ASCII)=="EGP2" && b.getShort(4).toInt()==2 && b.getInt(20)==0 && b.getInt(28)==0) { "EGP2 header" }
        val opcode=b.getShort(6).toInt() and 65535;val sequence=unsigned(b.getInt(8));val context=unsigned(b.getInt(12));val length=b.getInt(16)
        validate(opcode,sequence,context,length);require(bytes.size.toLong()==HEADER.toLong()+length)
        val payload=bytes.copyOfRange(HEADER,bytes.size);require(crc(header,payload)==b.getInt(24)) { "EGP2 CRC" }
        return Frame(opcode,sequence,context,payload)
    }
    private fun exact(input:InputStream,length:Int):ByteArray {
        val bytes=ByteArray(length);var at=0
        while(at<length) {
            val n=input.read(bytes,at,length-at)
            if(n<0)throw EOFException("Truncated EGP2 stream")
            if(n==0){val one=input.read();if(one<0)throw EOFException("Truncated EGP2 stream");bytes[at++]=one.toByte()}
            else at+=n
        };return bytes
    }
    fun read(input:InputStream):Frame {
        val header=exact(input,HEADER);val b=little(header)
        require(String(header,0,4,Charsets.US_ASCII)=="EGP2" && b.getShort(4).toInt()==2 && b.getInt(20)==0 && b.getInt(28)==0)
        val length=b.getInt(16);validate(b.getShort(6).toInt() and 65535,unsigned(b.getInt(8)),unsigned(b.getInt(12)),length)
        return decode(header+exact(input,length))
    }
    fun write(output:OutputStream,frame:Frame) { output.write(encode(frame));output.flush() }

    private fun proof(secret:ByteArray,run:ByteArray,client:ByteArray,server:ByteArray,role:String):ByteArray {
        require(secret.size==32 && run.size==16 && client.size==32 && server.size==32)
        val mac=Mac.getInstance("HmacSHA256");mac.init(SecretKeySpec(secret,"HmacSHA256"))
        mac.update(authMagic);mac.update(byteArrayOf(2,0,0,0));mac.update(run);mac.update(client);mac.update(server)
        return mac.doFinal(role.toByteArray(Charsets.US_ASCII))
    }
    fun authenticateHost(input:InputStream,output:OutputStream,secret:ByteArray,run:ByteArray,random:SecureRandom=SecureRandom()) {
        require(secret.size==32 && run.size==16)
        val client=ByteArray(32).also { random.nextBytes(it) }
        output.write(authMagic+byteArrayOf(2,0,0,0)+run+client);output.flush()
        val reply=exact(input,76)
        require(reply.copyOfRange(0,12).contentEquals(authMagic+byteArrayOf(2,0,0,0))) { "GPU authentication version" }
        val server=reply.copyOfRange(12,44)
        require(MessageDigest.isEqual(reply.copyOfRange(44,76),proof(secret,run,client,server,"server"))) { "GPU server authentication failed" }
        output.write(proof(secret,run,client,server,"client"));output.flush()
    }
    fun authenticateGuest(input:InputStream,output:OutputStream,secret:ByteArray,run:ByteArray,random:SecureRandom=SecureRandom()) {
        require(secret.size==32 && run.size==16)
        val hello=exact(input,60)
        require(hello.copyOfRange(0,12).contentEquals(authMagic+byteArrayOf(2,0,0,0)) &&
            MessageDigest.isEqual(hello.copyOfRange(12,28),run)) { "GPU run/version mismatch" }
        val client=hello.copyOfRange(28,60);val server=ByteArray(32).also { random.nextBytes(it) }
        output.write(authMagic+byteArrayOf(2,0,0,0)+server+proof(secret,run,client,server,"server"));output.flush()
        require(MessageDigest.isEqual(exact(input,32),proof(secret,run,client,server,"client"))) { "GPU host authentication failed" }
    }

    /** One stream producer per direction; duplicates are never executed twice. */
    class Sequence {
        private var last=0L
        fun accept(frame:Frame) { require(last<0xffffffffL && frame.sequence==last+1) { "EGP2 sequence gap/replay" };last=frame.sequence }
    }

    /** Publish an upload only after full contiguous delivery and digest match. */
    class Blob {
        private var id=0L;private var at=0;private var data:ByteArray?=null;private var expected=byteArrayOf()
        fun abort() { data=null;expected=byteArrayOf();at=0;id=0 }
        fun begin(blob:Long,size:Int,sha256:ByteArray) {
            if(data!=null || blob<=0 || size !in 1..MAX_BLOB || sha256.size!=32){abort();error("Invalid EGP2 blob begin")}
            id=blob;data=ByteArray(size);expected=sha256.copyOf();at=0
        }
        fun chunk(blob:Long,offset:Int,bytes:ByteArray) {
            val current=data
            if(current==null || blob!=id || offset!=at || bytes.isEmpty() || bytes.size>MAX_PAYLOAD-12 || bytes.size>current.size-at) {
                abort();error("Invalid EGP2 blob chunk")
            }
            bytes.copyInto(current,at);at+=bytes.size
        }
        fun commit(blob:Long):ByteArray {
            val current=data
            if(current==null || blob!=id || at!=current.size || !MessageDigest.isEqual(expected,MessageDigest.getInstance("SHA-256").digest(current))) {
                abort();error("Invalid/incomplete EGP2 blob commit")
            }
            abort();return current
        }
    }
}
