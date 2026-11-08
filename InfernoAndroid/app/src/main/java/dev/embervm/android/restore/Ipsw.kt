// SPDX-License-Identifier: GPL-3.0-or-later
// Streaming Kotlin port of IPSW.swift @ 4bc3970, with bounds/path/CRC validation.
package dev.embervm.android.restore

import dev.embervm.android.Plist
import java.io.*
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.zip.CRC32
import java.util.zip.Inflater
import java.util.zip.InflaterInputStream

class Ipsw(val file: File): Closeable {
    data class Member(val path: String,val method: Int,val compressedSize: Long,val plainSize: Long,val dataOffset: Long,val crc32: Long)
    data class StoredRange(val file: File,val offset: Long,val length: Long,val crc32: Long) {
        fun read(at: Long,count: Int): ByteArray {
            require(at>=0 && count>=0 && at<=length-count) { "OS range out of bounds" }
            return RandomAccessFile(file,"r").use { r->r.seek(offset+at);ByteArray(count).also { r.readFully(it) } }
        }
    }
    val members: Map<String,Member> = directory()
    private fun directory(): Map<String,Member> = RandomAccessFile(file,"r").use { r ->
        val size=r.length();require(size>=22) { "Not a ZIP archive" }
        val tailAt=(size-65557).coerceAtLeast(0);val tail=read(r,tailAt,(size-tailAt).toInt())
        val end=(tail.size-22 downTo 0).firstOrNull { u32(tail,it)==0x06054b50L && it+22+u16(tail,it+20)==tail.size } ?: error("ZIP end record missing")
        require(u16(tail,end+4)==0 && u16(tail,end+6)==0) { "Multi-disk ZIP unsupported" }
        var count=u16(tail,end+10).toLong();var length=u32(tail,end+12);var offset=u32(tail,end+16)
        if(count==65535L || length==0xffffffffL || offset==0xffffffffL) {
            val locatorAt=tailAt+end-20;require(locatorAt>=0)
            val locator=read(r,locatorAt,20);require(u32(locator,0)==0x07064b50L && u32(locator,4)==0L && u32(locator,16)==1L) { "ZIP64 locator" }
            val z=read(r,u64(locator,8),56);require(u32(z,0)==0x06064b50L && u32(z,16)==0L && u32(z,20)==0L) { "ZIP64 record" }
            count=u64(z,32);length=u64(z,40);offset=u64(z,48)
        }
        require(count in 1..100000 && length<=32L*1024*1024 && offset<=size-length && length>=46) { "ZIP directory bounds" }
        val cd=read(r,offset,length.toInt());var at=0;val found=linkedMapOf<String,Member>()
        repeat(count.toInt()) {
            require(at<=cd.size-46 && u32(cd,at)==0x02014b50L) { "ZIP central directory" }
            val flags=u16(cd,at+8);val method=u16(cd,at+10);val crc=u32(cd,at+16)
            require(flags and 1==0 && method in listOf(0,8)) { "Encrypted/unsupported ZIP member" }
            var compressed=u32(cd,at+20);var plain=u32(cd,at+24);var local=u32(cd,at+42)
            val nameLen=u16(cd,at+28);val extraLen=u16(cd,at+30);val commentLen=u16(cd,at+32)
            require(u16(cd,at+34)==0 && nameLen>0 && nameLen.toLong()+extraLen+commentLen <= cd.size-at-46) { "ZIP entry bounds" }
            val name=String(cd,at+46,nameLen,Charsets.UTF_8);safePath(name)
            val extraEnd=at+46+nameLen+extraLen;var e=at+46+nameLen
            while(e<extraEnd) {
                require(e<=extraEnd-4);val tag=u16(cd,e);val n=u16(cd,e+2);val finish=e+4+n;require(finish<=extraEnd)
                if(tag==1) {
                    var v=e+4
                    fun next(): Long { require(v<=finish-8);return u64(cd,v).also { v+=8 } }
                    if(plain==0xffffffffL) plain=next()
                    if(compressed==0xffffffffL) compressed=next()
                    if(local==0xffffffffL) local=next()
                }
                e=finish
            }
            at=extraEnd+commentLen
            require(plain!=0xffffffffL && compressed!=0xffffffffL && local!=0xffffffffL) { "ZIP64 field missing" }
            val h=read(r,local,30);require(u32(h,0)==0x04034b50L && u16(h,8)==method) { "ZIP local header" }
            val localName=read(r,local+30,u16(h,26));require(localName.contentEquals(name.toByteArray(Charsets.UTF_8))) { "ZIP filename mismatch" }
            val data=Math.addExact(local,30L+u16(h,26)+u16(h,28))
            require(compressed<=size && data<=size-compressed && data+compressed<=offset) { "ZIP payload bounds" }
            require(!found.containsKey(name)) { "Duplicate ZIP member" }
            if(!name.endsWith('/')) found[name]=Member(name,method,compressed,plain,data,crc)
        }
        require(at==cd.size) { "Unexpected ZIP directory bytes" };found
    }
    fun member(path: String) = members[path] ?: error("В IPSW нет $path")
    fun read(path: String,maxBytes: Int=256*1024*1024): ByteArray {
        val m=member(path);require(m.plainSize<=maxBytes) { "Component too large for memory; stream it" }
        return ByteArrayOutputStream(m.plainSize.toInt()).use { out->transfer(m,out);out.toByteArray() }
    }
    fun extract(path: String,destination: File) {
        destination.parentFile?.mkdirs();val temp=File(destination.path+".part")
        try { temp.outputStream().buffered().use { transfer(member(path),it) };Files.move(temp.toPath(),destination.toPath(),StandardCopyOption.REPLACE_EXISTING) }
        finally { temp.delete() }
    }
    private fun transfer(m: Member,out: OutputStream) {
        RandomAccessFile(file,"r").use { r ->
            r.seek(m.dataOffset)
            val bounded=object: InputStream() {
                var remaining=m.compressedSize
                override fun read(): Int { if(remaining==0L)return -1;val b=r.read();if(b<0)throw EOFException();remaining--;return b }
                override fun read(b: ByteArray,o: Int,n: Int): Int { if(n==0)return 0;if(remaining==0L)return -1;val k=r.read(b,o,minOf(n.toLong(),remaining).toInt());if(k<0)throw EOFException();remaining-=k;return k }
            }
            val inflater=if(m.method==8)Inflater(true) else null
            val input=if(inflater!=null)InflaterInputStream(bounded,inflater,65536) else bounded
            try {
                val buffer=ByteArray(1024*1024);val crc=CRC32();var total=0L
                while(true) { val n=input.read(buffer);if(n<0)break;total+=n;require(total<=m.plainSize) { "ZIP inflated size overflow" };crc.update(buffer,0,n);out.write(buffer,0,n) }
                require(total==m.plainSize && crc.value==m.crc32) { "ZIP size/CRC mismatch: ${m.path}" }
                require(bounded.remaining==0L && (inflater==null || inflater.finished() && inflater.remaining==0)) { "ZIP compressed payload mismatch" }
            } finally { inflater?.end() }
        }
    }
    fun rangeOfStored(path: String): StoredRange = member(path).let { require(it.method==0 && it.compressedSize==it.plainSize) { "ASR requires a stored OS image" };StoredRange(file,it.dataOffset,it.plainSize,it.crc32) }
    @Suppress("UNCHECKED_CAST")
    fun buildIdentity(): Map<String,Any?> {
        val manifest=Plist.parse(read("BuildManifest.plist",32*1024*1024).inputStream()) as? Map<String,Any?> ?: error("BuildManifest is not XML")
        require(manifest["ProductBuildVersion"]=="18A5351d") { "Нужен iOS 14.0 beta 5 (18A5351d)" }
        val identities=manifest["BuildIdentities"] as? List<*> ?: error("No BuildIdentities")
        return identities.mapNotNull { it as? Map<String,Any?> }.firstOrNull {
            val info=it["Info"] as? Map<*,*>
            info?.get("DeviceClass")=="n104ap" && (info["Variant"] as? String)?.contains("Erase")==true
        } ?: error("Нет Erase identity для iPhone12,1 / n104ap")
    }
    override fun close() = Unit // Each operation owns its file handle; concurrent range reads cannot race a seek.
    companion object {
        fun path(component: String,identity: Map<String,Any?>): String? = (((identity["Manifest"] as? Map<*,*>)?.get(component) as? Map<*,*>)?.get("Info") as? Map<*,*>)?.get("Path") as? String
        fun safePath(path: String) { require(path.isNotEmpty() && !path.startsWith('/') && '\\' !in path && ':' !in path && '\u0000' !in path && path.removeSuffix("/").split('/').all { it.isNotEmpty() } && path.split('/').none { it==".." || it=="." }) { "Unsafe ZIP path" } }
        private fun read(r: RandomAccessFile,at: Long,n: Int): ByteArray { require(at>=0 && n>=0 && at<=r.length()-n) { "ZIP truncated" };r.seek(at);return ByteArray(n).also { r.readFully(it) } }
        private fun u16(b: ByteArray,o: Int) = (b[o].toInt() and 255) or ((b[o+1].toInt() and 255) shl 8)
        private fun u32(b: ByteArray,o: Int) = (0..3).fold(0L){ n,i->n or ((b[o+i].toLong() and 255) shl (i*8)) }
        private fun u64(b: ByteArray,o: Int): Long { require(b[o+7]>=0) { "ZIP64 integer overflow" };return (0..7).fold(0L){n,i->n or ((b[o+i].toLong() and 255) shl(i*8))} }
    }
}
