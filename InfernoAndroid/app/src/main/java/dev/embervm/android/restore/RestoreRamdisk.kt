// SPDX-License-Identifier: GPL-3.0-or-later
// Port of RestoreRamdisk.swift, MakrSas/Inferno-iOS @ 4bc3970.
package dev.embervm.android.restore

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Replaces existing catalog records; never inserts into the HFS B-tree. */
object RestoreRamdisk {
    const val daemonPath = "/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-01.30-006-P.bin"
    const val patcherPath = "/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-11.04-012.2-P.bin"
    const val jobPath = "/System/Library/LaunchDaemons/com.apple.syslogd.plist"
    const val restoreMonitorJobPath = "/System/Library/LaunchDaemons/com.apple.restored_external.plist"
    const val storageJobPath = "/System/Library/LaunchDaemons/com.apple.KeyMaker.plist"
    val disabledRestoreMonitorJob:ByteArray get()=PlistXml.encode(mapOf(
        "Label" to "com.apple.restored_external", "ProgramArguments" to listOf("/usr/local/bin/restored_external"),
        "Disabled" to true, "RunAtLoad" to false, "KeepAlive" to false
    ))
    // Restore mode exposes only a dummy root disk until the NAND firmware
    // handshake completes. This is the same non-flashing --skip invocation
    // observed in restored's successful update_NAND step on 18A5351d. Repurpose
    // the existing KeyMaker job (its original executable isn't in this ramdisk).
    val storageJob:ByteArray get()=PlistXml.encode(mapOf(
        "Label" to "dev.embervm.storage-ready",
        "ProgramArguments" to listOf("/usr/bin/nvmefwupdater","--skip"),
        "RunAtLoad" to true,"KeepAlive" to mapOf("SuccessfulExit" to false),
        "ThrottleInterval" to 5,"StandardOutPath" to "/dev/console","StandardErrorPath" to "/dev/console"
    ))
    val job: ByteArray get() = ("""
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
        \t<key>Label</key>
        \t<string>com.inferno.patcher</string>
        \t<key>ProgramArguments</key>
        \t<array>
        \t\t<string>$daemonPath</string>
        \t</array>
        \t<key>RunAtLoad</key>
        \t<true/>
        \t<key>StandardOutPath</key>
        \t<string>/dev/console</string>
        \t<key>StandardErrorPath</key>
        \t<string>/dev/console</string>
        </dict>
        </plist>
    """.trimIndent().replace("\\t", "\t") + "\n").toByteArray()

    fun build(stock: File, destination: File, daemon: ByteArray, patcher: ByteArray) {
        require(daemon.isNotEmpty() && patcher.isNotEmpty()) { "Missing guest programs" }
        rewrite(stock,destination) { volume ->
            volume.replace(jobPath,job,0x1a4)
            volume.replace(daemonPath,daemon,0x1ed)
            volume.replace(patcherPath,patcher,0x1ed)
        }
    }
    /** Patch-only ramdisk: never launch restored_external or its /sbin/reboot. */
    fun buildPatchBoot(injected:File,destination:File,daemon:ByteArray) {
        require(daemon.isNotEmpty()) { "Нет обновлённого демона патчей" }
        rewrite(injected,destination) {
            it.replace(restoreMonitorJobPath,disabledRestoreMonitorJob,0x1a4)
            it.replace(storageJobPath,storageJob,0x1a4)
            // Replace even a previously prepared ramdisk: old daemon scans by
            // prefix and may select .InfernoOriginalBytes on the second boot.
            it.replace(daemonPath,daemon,0x1ed)
        }
    }
    /** Isolated offscreen GPU probe. No restored process or filesystem patcher. */
    fun buildGpuProbe(stock:File,destination:File,probe:ByteArray,library:ByteArray) {
        require(probe.isNotEmpty() && library.isNotEmpty()) { "Нет программ GPU-теста" }
        rewrite(stock,destination) {
            it.replace(jobPath,job,0x1a4)
            it.replace(daemonPath,probe,0x1ed)
            it.replace(patcherPath,library,0x1ed)
            it.replace(restoreMonitorJobPath,disabledRestoreMonitorJob,0x1a4)
            it.replace(storageJobPath,storageJob,0x1a4)
        }
    }
    private fun rewrite(stock:File,destination:File,edit:(Volume)->Unit) {
        require(stock.canonicalFile != destination.canonicalFile)
        val temp = File(destination.path + ".part")
        try {
            stock.inputStream().use { input -> temp.outputStream().use { input.copyTo(it, 1 shl 20) } }
            RandomAccessFile(temp,"rw").use { file ->
                val payload = payloadRange(file)
                val volume = Volume(file,payload.first,payload.second)
                edit(volume)
                file.fd.sync()
            }
            java.nio.file.Files.move(temp.toPath(),destination.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING)
        } finally { temp.delete() }
    }
    internal fun payloadRange(file: RandomAccessFile): Pair<Long,Long> {
        file.seek(0)
        fun element(): Pair<Int,Long> {
            val tag=file.readUnsignedByte();var length=file.readUnsignedByte().toLong()
            if (length and 128 != 0L) {
                val n=(length and 127).toInt();require(n in 1..4);length=0
                repeat(n) { length=(length shl 8) or file.readUnsignedByte().toLong() }
            }
            require(length <= file.length()-file.filePointer) { "Truncated IM4P" }
            return tag to length
        }
        val outer=element(); require(outer.first==0x30 && outer.second==file.length()-file.filePointer)
        val magic=element();require(magic.first==0x16 && magic.second==4L)
        val b=ByteArray(4);file.readFully(b);require(String(b)=="IM4P")
        repeat(2) { val e=element();require(e.first==0x16);file.seek(file.filePointer+e.second) }
        val data=element();require(data.first==4)
        return file.filePointer to data.second
    }
    private class Volume(val file:RandomAccessFile,val base:Long,val length:Long) {
        data class Extent(val start:Long,val count:Long)
        data class Fork(val size:Long,val blocks:Long,val extents:List<Extent>)
        data class Entry(val folder:Boolean,val id:Long,val at:Long,val fork:Fork?)
        val header=read(base+1024,512)
        val block=u32(header,40)
        val total=u32(header,44)
        val allocation:Fork
        val catalog:Fork
        val nodeSize:Int
        val firstLeaf:Long
        init {
            require(String(header.copyOfRange(0,2)) in listOf("H+","HX")) { "Ramdisk payload is not raw HFS+/HFSX" }
            require(block in 512..65536 && block and (block-1)==0L && total>0 && total<=length/block)
            allocation=fork(header,112);catalog=fork(header,272)
            val h=readFork(catalog,0,512)
            nodeSize=u16(h,32);firstLeaf=u32(h,24)
            require(nodeSize in 512..32768 && nodeSize and (nodeSize-1)==0)
        }
        fun replace(path:String,data:ByteArray,mode:Int) {
            var parent=2L;var entry:Entry?=null
            val parts=path.split('/').filter { it.isNotEmpty() }
            for ((index,name) in parts.withIndex()) {
                entry=lookup(parent,name) ?: error("HFS: missing $path")
                require(index==parts.lastIndex || entry.folder) { "HFS: non-directory path" }
                parent=entry.id
            }
            val e=entry ?: error("HFS: empty path");require(!e.folder)
            val f=e.fork!!;var extents=f.extents
            if (extents.sumOf { it.count*block } < data.size) {
                val needed=(data.size+block-1)/block
                val start=allocate(needed);extents=listOf(Extent(start,needed))
                write(e.at+100,be(needed,4))
                write(e.at+104,be(start,4)+be(needed,4)+ByteArray(56))
                // Old extents remain allocated: this ramdisk is used once.
            }
            writeFork(Fork(data.size.toLong(),extents.sumOf { it.count },extents),0,data)
            write(e.at+88,be(data.size.toLong(),8));write(e.at+42,be((mode or 0x8000).toLong(),2))
            write(e.at+32,ByteArray(8));write(e.at+41,byteArrayOf(0)) // root/root, clear UF_COMPRESSED
        }
        fun allocate(count:Long):Long {
            require(count>0)
            val bitmap=readFork(allocation,0,((total+7)/8).toInt())
            var run=0L;var start=0L
            for (i in 0 until total) {
                if (bitmap[(i/8).toInt()].toInt() and (128 ushr (i%8).toInt()) !=0) { run=0;continue }
                if (run==0L) start=i
                if (++run!=count) continue
                val free=u32(read(base+1072,4),0);require(free>=count) { "HFS: bad free-block count" }
                for (b in start until start+count) bitmap[(b/8).toInt()]=(bitmap[(b/8).toInt()].toInt() or (128 ushr (b%8).toInt())).toByte()
                writeFork(allocation,0,bitmap);write(base+1072,be(free-count,4));return start
            }
            error("HFS: no contiguous free blocks")
        }
        fun lookup(parent:Long,name:String):Entry? {
            var index=firstLeaf;val visited=HashSet<Long>();val chars=name.toCharArray()
            while (index!=0L) {
                require(visited.add(index) && visited.size<=catalog.size/nodeSize) { "HFS: catalog leaf cycle" }
                val logical=index*nodeSize;val node=readFork(catalog,logical,nodeSize)
                require(node[8].toInt()==-1) { "HFS: not a leaf" }
                val records=u16(node,10);require(records<=(nodeSize-14)/2)
                for (r in 0 until records) {
                    val off=u16(node,nodeSize-(r+1)*2);require(off>=14 && off+8<=nodeSize-(records+1)*2)
                    val kl=u16(node,off);require(kl>=6 && off+2+kl<=nodeSize-(records+1)*2)
                    val n=u16(node,off+6);require(n<=255 && 6+2*n<=kl)
                    if (u32(node,off+2)!=parent || n!=chars.size || (0 until n).any { u16(node,off+8+it*2)!=chars[it].code }) continue
                    val at=(off+2+kl+1) and -2;val type=u16(node,at)
                    require(at+(if(type==2)248 else 88)<=nodeSize-(records+1)*2)
                    // Metadata writes must not cross an extent. Refuse unsupported layouts.
                    val physical=offset(catalog,logical+at)
                    require(offset(catalog,logical+at+(if(type==2)247 else 87))==physical+(if(type==2)247 else 87))
                    if(type==1 || type==2) return Entry(type==1,u32(node,at+8),physical,if(type==2)fork(node,at+88) else null)
                }
                index=u32(node,0)
            }
            return null
        }
        fun fork(bytes:ByteArray,at:Int):Fork {
            val size=u64(bytes,at);val blocks=u32(bytes,at+12)
            val extents=(0..7).map { Extent(u32(bytes,at+16+it*8),u32(bytes,at+20+it*8)) }.filter { it.count>0 }
            require(extents.all { it.start<total && it.count<=total-it.start })
            require(extents.sumOf { it.count }==blocks && size<=blocks*block) { "HFS: overflow extents unsupported" }
            return Fork(size,blocks,extents)
        }
        fun offset(f:Fork,logical:Long):Long {
            require(logical>=0)
            var skip=logical
            for(e in f.extents) { val size=e.count*block;if(skip<size)return base+e.start*block+skip;skip-=size }
            error("HFS: fork range out of bounds")
        }
        fun readFork(f:Fork,at:Long,n:Int):ByteArray {
            require(at>=0 && n>=0 && at+n<=f.size)
            val out=ByteArray(n);var done=0
            while(done<n) { val p=offset(f,at+done);val chunk=minOf(n-done,(block-(p-base)%block).toInt());read(p,chunk).copyInto(out,done);done+=chunk }
            return out
        }
        fun writeFork(f:Fork,at:Long,bytes:ByteArray) {
            require(at>=0 && at+bytes.size<=f.extents.sumOf { it.count*block })
            var done=0
            while(done<bytes.size) { val p=offset(f,at+done);val chunk=minOf(bytes.size-done,(block-(p-base)%block).toInt());write(p,bytes.copyOfRange(done,done+chunk));done+=chunk }
        }
        fun read(at:Long,n:Int):ByteArray { require(at>=base && n>=0 && at-base<=length-n);file.seek(at);return ByteArray(n).also { file.readFully(it) } }
        fun write(at:Long,b:ByteArray) { require(at>=base && at-base<=length-b.size);file.seek(at);file.write(b) }
    }
    private fun u16(b:ByteArray,p:Int):Int { require(p>=0 && p+2<=b.size);return ((b[p].toInt() and 255) shl 8) or (b[p+1].toInt() and 255) }
    private fun u32(b:ByteArray,p:Int):Long { require(p>=0 && p+4<=b.size);return (0..3).fold(0L) { a,i -> (a shl 8) or (b[p+i].toLong() and 255) } }
    private fun u64(b:ByteArray,p:Int):Long { require(p>=0 && p+8<=b.size);return ByteBuffer.wrap(b,p,8).order(ByteOrder.BIG_ENDIAN).long.also { require(it>=0) } }
    private fun be(v:Long,n:Int)=ByteArray(n) { (v ushr ((n-1-it)*8)).toByte() }
}
