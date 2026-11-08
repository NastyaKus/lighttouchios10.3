// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/** Verify embedded ad-hoc CodeDirectory page hashes and embedded special slots.
 * This checks integrity, not Apple's authorization or physical-device signing. */
object SystemCodeSignature {
    fun verify(bytes:ByteArray) {
        SystemMachO.inspect(bytes)
        val little=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);var command=32;var signature=-1
        var embeddedInfo:Pair<Int,Int>?=null
        repeat(little.getInt(16)) {
            if(little.getInt(command)==0x1d)signature=little.getInt(command+8)
            if(little.getInt(command)==0x19)repeat(little.getInt(command+64)) { index ->
                val section=command+72+index*80
                val name=String(bytes,section,16,Charsets.US_ASCII).trimEnd('\u0000')
                val segment=String(bytes,section+16,16,Charsets.US_ASCII).trimEnd('\u0000')
                if(name=="__info_plist" && segment=="__TEXT") {
                    require(embeddedInfo==null);embeddedInfo=little.getInt(section+48) to little.getLong(section+40).toInt()
                }
            }
            command+=little.getInt(command+4)
        }
        require(signature>=0);val b=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        val length=b.getInt(signature+4);val count=b.getInt(signature+8)
        val blobs=linkedMapOf<Int,Pair<Int,Int>>()
        repeat(count) {
            val type=b.getInt(signature+12+it*8);val pos=signature+b.getInt(signature+16+it*8)
            require(!blobs.containsKey(type));blobs[type]=pos to b.getInt(pos+4)
        }
        var directories=0
        for((type,blob) in blobs)if(type==0 || type in 0x1000..0x1005) {
            val (at,size)=blob
            require(b.getInt(at)==0xfade0c02.toInt() && size>=44)
            require(b.getInt(at+12) and 2 !=0) { "Expected ad-hoc CodeDirectory" }
            val hashes=b.getInt(at+16);val special=b.getInt(at+24);val pages=b.getInt(at+28)
            val limit=b.getInt(at+32).toLong() and 0xffffffffL
            val hashSize=bytes[at+36].toInt() and 255;val hashType=bytes[at+37].toInt() and 255
            val shift=bytes[at+39].toInt() and 255
            require(shift in 9..16 && special in 0..16 && pages in 1..131072 && limit==signature.toLong()) { "CodeDirectory coverage" }
            val algorithm=when(hashType){1->"SHA-1";2,3->"SHA-256";4->"SHA-384";else->error("Unsupported signature hash type")}
            val expectedSize=when(hashType){1,3->20;2->32;else->48};require(hashSize==expectedSize)
            require(hashes>=44L+special.toLong()*hashSize && hashes.toLong()+pages.toLong()*hashSize<=size)
            val pageSize=1 shl shift;require(pages.toLong()==(limit+pageSize-1)/pageSize)
            if(b.getInt(at+8)>=0x20100)require(size>=48 && b.getInt(at+44)==0) { "Scatter signatures are unsupported" }
            val digest=MessageDigest.getInstance(algorithm)
            fun matches(hashAt:Int,start:Int,len:Int) {
                digest.update(bytes,start,len);val value=digest.digest()
                require((0 until hashSize).all { bytes[hashAt+it]==value[it] }) { "CodeDirectory hash mismatch" }
            }
            repeat(pages) { page -> val start=page*pageSize;matches(at+hashes+page*hashSize,start,minOf(pageSize,signature-start)) }
            for(slot in 1..special) {
                val hashAt=at+hashes-slot*hashSize;val value=blobs[slot] ?: if(slot==1)embeddedInfo else null
                if(value!=null)matches(hashAt,value.first,value.second)
                else require((0 until hashSize).all { bytes[hashAt+it]==0.toByte() }) { "Unverifiable external signing slot" }
            }
            directories++
        }
        require(directories>0 && signature+length<=bytes.size)
    }
}
