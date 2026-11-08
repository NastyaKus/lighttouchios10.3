// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import java.security.MessageDigest

/** Exact arm64/arm64e executable editing; never grows/shifts executable segments.
 * The result is UNSIGNED until the native signer succeeds and is verified. */
object SystemMachO {
    data class Identity(val uuid:String,val cpuSubtype:Int,val firstSection:Int,val commandEnd:Int,
        val commands:Int,val dylibs:List<String>,val identifier:String,val entitlements:ByteArray,
        val flags:Int,val platform:Int)
    data class Prepared(val bytes:ByteArray,val identity:Identity,val dependency:String)
    private const val MAGIC=0xfeedfacf.toInt()
    private fun view(bytes:ByteArray)=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
    private fun range(bytes:ByteArray,at:Long,size:Long) { require(at>=0 && size>=0 && at<=bytes.size && size<=bytes.size-at) { "Mach-O range" } }
    private fun string(bytes:ByteArray,at:Int,end:Int):String {
        require(at in 0 until end && end<=bytes.size)
        var stop=at;while(stop<end && bytes[stop]!=0.toByte())stop++
        require(stop<end) { "Unterminated Mach-O string" }
        return bytes.copyOfRange(at,stop).toString(Charsets.UTF_8)
    }
    fun inspect(bytes:ByteArray):Identity {
        require(bytes.size in 32..64*1024*1024) { "Mach-O size" }
        val b=view(bytes);require(b.getInt(0)==MAGIC && b.getInt(4)==0x100000c && b.getInt(12)==2) { "Expected arm64 executable" }
        val count=b.getInt(16);val commandSize=b.getInt(20)
        require(count in 1..4096 && commandSize>=0);range(bytes,32,commandSize.toLong())
        val end=32+commandSize;var at=32;var first=bytes.size;var uuid:String?=null;var signatureAt=-1;var signatureSize=0
        val dylibs=mutableListOf<String>()
        repeat(count) {
            require(end-at>=8);val cmd=b.getInt(at);val size=b.getInt(at+4)
            require(size>=8 && size%8==0 && size<=end-at) { "Mach-O command bounds" }
            when(cmd) {
                0x19 -> {
                    require(size>=72);val sections=b.getInt(at+64)
                    require(sections in 0..128 && 72L+sections*80<=size)
                    range(bytes,b.getLong(at+40),b.getLong(at+48))
                    repeat(sections) { i ->
                        val section=at+72+i*80;val offset=b.getInt(section+48).toLong() and 0xffffffffL
                        val length=b.getLong(section+40);val kind=b.getInt(section+64) and 255
                        if(length>0 && kind !in setOf(1,12,18)) {
                            range(bytes,offset,length);require(offset>=end) { "Section overlaps load commands" };first=minOf(first,offset.toInt())
                        }
                    }
                }
                0x1b -> { require(size==24 && uuid==null);val u=ByteBuffer.wrap(bytes,at+8,16);uuid=UUID(u.long,u.long).toString() }
                0x1d -> { require(size==16 && signatureAt<0);signatureAt=b.getInt(at+8);signatureSize=b.getInt(at+12);range(bytes,signatureAt.toLong(),signatureSize.toLong()) }
                0xc,0x80000018.toInt(),0x8000001f.toInt(),0x80000023.toInt() -> {
                    require(size>=24);val name=b.getInt(at+8);require(name in 24 until size);dylibs+=string(bytes,at+name,at+size)
                }
            }
            at+=size
        }
        require(at==end && first>=end && uuid!=null && signatureAt>=first && signatureSize>=12) { "Incomplete executable metadata" }
        val s=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        require(s.getInt(signatureAt)==0xfade0cc0.toInt()) { "Expected embedded signature" }
        val blobSize=s.getInt(signatureAt+4);val blobs=s.getInt(signatureAt+8)
        require(blobSize in 12..signatureSize && blobs in 1..128 && 12L+blobs*8<=blobSize)
        var entitlements=byteArrayOf();var identifier:String?=null;var flags=0;var platform=0
        repeat(blobs) { index ->
            val slot=s.getInt(signatureAt+12+index*8);val offset=s.getInt(signatureAt+16+index*8)
            require(offset>=12+blobs*8 && offset<=blobSize-8)
            val pos=signatureAt+offset;val len=s.getInt(pos+4);require(len>=8 && len<=blobSize-offset)
            if(slot==0) {
                require(s.getInt(pos)==0xfade0c02.toInt() && len>=44 && identifier==null)
                val name=s.getInt(pos+20);require(name in 44 until len);identifier=string(bytes,pos+name,pos+len)
                flags=s.getInt(pos+12);platform=bytes[pos+38].toInt() and 255
            }
            if(slot==5) { require(s.getInt(pos)==0xfade7171.toInt() && entitlements.isEmpty());entitlements=bytes.copyOfRange(pos+8,pos+len) }
        }
        require(identifier!=null) { "Missing CodeDirectory" }
        return Identity(uuid!!,b.getInt(8),first,end,count,dylibs,identifier!!,entitlements,flags,platform)
    }

    fun inject(bytes:ByteArray,path:String,expectedUuid:String,expectedSha256:String):Prepared {
        require(expectedSha256.matches(Regex("[a-f0-9]{64}")) &&
            MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }==expectedSha256) { "System executable SHA-256 differs from the ABI manifest" }
        val identity=inspect(bytes)
        require(identity.uuid==expectedUuid) { "System executable UUID differs from the ABI manifest" }
        require(path.startsWith("/usr/lib/ember/") && path.endsWith(".dylib") && !path.contains("..") &&
            path.all { it.code in 33..126 && it !in "\\," }) { "Invalid shim path" }
        require(path !in identity.dylibs) { "System shim already present" }
        val name=path.toByteArray(Charsets.UTF_8);require(name.size<=512)
        val size=(24+name.size+1+7) and -8
        require(size<=identity.firstSection-identity.commandEnd) { "Insufficient verified Mach-O header padding" }
        require((identity.commandEnd until identity.commandEnd+size).all { bytes[it]==0.toByte() }) { "Nonzero Mach-O padding" }
        val copy=bytes.copyOf();val b=view(copy);val at=identity.commandEnd
        b.putInt(at,0xc);b.putInt(at+4,size);b.putInt(at+8,24)
        // timestamp/current/compatibility version are zero; bytes were verified zero.
        name.copyInto(copy,at+24)
        b.putInt(16,identity.commands+1);b.putInt(20,identity.commandEnd-32+size)
        val check=inspect(copy);require(check.dylibs.last()==path && check.entitlements.contentEquals(identity.entitlements))
        return Prepared(copy,identity,path)
    }
}
