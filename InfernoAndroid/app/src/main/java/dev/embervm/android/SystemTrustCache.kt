// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import dev.embervm.android.restore.Img4
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/** Exact v1 trust-cache preparation for the copied emulated guest. This adds
 * specific CodeDirectory hashes, never an allow-all rule or Apple AP ticket.
 * A cdhash is an identifier; extracting it alone does not verify code pages. */
object SystemTrustCache {
    data class Entry(val hash:ByteArray,val hashType:Int,val flags:Int=0)
    data class Cache(val uuid:ByteArray,val entries:List<Entry>)
    private val order=Comparator<Entry> { a,b ->
        var result=0
        for(i in 0 until 20) { result=(a.hash[i].toInt() and 255)-(b.hash[i].toInt() and 255);if(result!=0)break }
        result
    }
    private fun validate(entry:Entry) { require(entry.hash.size==20 && entry.hashType in 1..4 && entry.flags in 0..255) }
    private fun sorted(entries:List<Entry>)=entries.zipWithNext().all { (a,b) ->
        val compare=order.compare(a,b)
        compare<0 || (compare==0 && a.hashType==b.hashType && a.flags==b.flags)
    }
    fun decode(bytes:ByteArray):Cache {
        require(bytes.size in 24..2*1024*1024)
        val b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        require(b.int==1) { "Only iOS 14 trust-cache v1 is supported" }
        val uuid=ByteArray(16).also { b.get(it) };val count=b.int
        require(count in 0..65536 && bytes.size.toLong()==24L+count*22L) { "Trust-cache count/size" }
        val entries=List(count) {
            Entry(ByteArray(20).also { b.get(it) },b.get().toInt() and 255,b.get().toInt() and 255).also(::validate)
        }
        // Apple's stock 18A5351d rtsc has one identical duplicate. Preserve it;
        // ambiguous duplicate metadata and descending order are still rejected.
        require(sorted(entries)) { "Unsorted trust-cache or conflicting duplicate cdhash" }
        return Cache(uuid,entries)
    }
    fun encode(cache:Cache):ByteArray {
        require(cache.uuid.size==16 && cache.entries.size<=65536)
        cache.entries.forEach(::validate)
        val entries=cache.entries.sortedWith(order)
        require(sorted(entries)) { "Conflicting duplicate cdhash" }
        return ByteBuffer.allocate(24+entries.size*22).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(1);put(cache.uuid);putInt(entries.size)
            for(entry in entries){put(entry.hash);put(entry.hashType.toByte());put(entry.flags.toByte())}
        }.array()
    }
    fun augmentIM4P(im4p:ByteArray,additions:List<Entry>):ByteArray {
        require(im4p.size<=2*1024*1024 && additions.size in 1..64)
        val payload=Img4.readIM4P(im4p)
        require(payload.type=="rtsc" && payload.extras.isEmpty()) { "Expected plain restore trust-cache IM4P" }
        val cache=decode(payload.data);val entries=cache.entries.toMutableList()
        additions.forEach { entry ->
            validate(entry)
            val old=entries.firstOrNull { it.hash.contentEquals(entry.hash) }
            if(old==null)entries+=entry
            else require(old.hashType==entry.hashType && old.flags==entry.flags) { "Conflicting trust-cache metadata" }
        }
        if(entries.size==cache.entries.size)return im4p.copyOf()
        val data=encode(Cache(cache.uuid,entries))
        // Deterministic distinct module UUID. Not a signing key or authorization.
        val uuid=MessageDigest.getInstance("SHA-256").digest(data).copyOfRange(0,16)
        val result=Img4.makeIM4P(payload.type,payload.version,encode(Cache(uuid,entries)))
        require(decode(Img4.readIM4P(result).data).entries.size==entries.size)
        return result
    }

    /** Bounded Mach-O signature directory extraction, supports executables and
     * dylibs. Hash the complete CodeDirectory and truncate to 20 bytes as XNU
     * expects. No native pointer, filesystem write or CMS trust is involved. */
    fun codeDirectoryHashes(image:ByteArray):List<Entry> {
        require(image.size in 32..64*1024*1024)
        val b=ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN)
        require(b.getInt(0)==0xfeedfacf.toInt() && b.getInt(4)==0x100000c && b.getInt(12) in listOf(2,6))
        val count=b.getInt(16);val commands=b.getInt(20)
        require(count in 1..4096 && commands>=0 && commands<=image.size-32)
        var at=32;val end=32+commands;var signature=-1;var region=0
        repeat(count) {
            require(end-at>=8);val type=b.getInt(at);val size=b.getInt(at+4)
            require(size>=8 && size%8==0 && size<=end-at)
            if(type==0x1d) {
                require(size==16 && signature<0);signature=b.getInt(at+8);region=b.getInt(at+12)
                require(signature>=end && region>=12 && signature<=image.size && region<=image.size-signature)
            }
            at+=size
        }
        require(at==end && signature>=0)
        val s=ByteBuffer.wrap(image).order(ByteOrder.BIG_ENDIAN)
        val length=s.getInt(signature+4);val blobs=s.getInt(signature+8)
        require(s.getInt(signature)==0xfade0cc0.toInt() && length in 12..region && blobs in 1..128 && 12L+blobs*8<=length)
        val results=mutableListOf<Entry>();val slots=mutableSetOf<Int>()
        repeat(blobs) { index ->
            val slot=s.getInt(signature+12+index*8);val offset=s.getInt(signature+16+index*8)
            require(slots.add(slot) && offset>=12+blobs*8 && offset<=length-8)
            val pos=signature+offset;val size=s.getInt(pos+4);require(size>=8 && size<=length-offset)
            if(slot==0 || slot in 0x1000..0x1005) {
                require(s.getInt(pos)==0xfade0c02.toInt() && size>=44)
                val type=image[pos+37].toInt() and 255
                val algorithm=when(type){1->"SHA-1";2,3->"SHA-256";4->"SHA-384";else->error("Unsupported CodeDirectory hash")}
                val digest=MessageDigest.getInstance(algorithm).digest(image.copyOfRange(pos,pos+size))
                results+=Entry(digest.copyOfRange(0,20),type)
            }
        }
        require(slots.contains(0) && results.isNotEmpty());return results
    }
}
