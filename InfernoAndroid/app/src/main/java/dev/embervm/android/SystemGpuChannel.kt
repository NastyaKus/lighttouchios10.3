// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.nio.file.attribute.PosixFilePermissions
import java.util.zip.CRC32

/** What a guest root broker needs to reach the per-boot system GPU host:
 * the loopback port (slirp gateway 10.0.2.2 inside the guest), the run
 * identity and the 32-byte capability. One 4096-byte block, written 0600 to
 * the app's private directory for the life of one VM boot and wiped after.
 *
 *     0  "EGC1"   4  u32 version=1   8  u32 port   12 u32 flags=0
 *     16 run[16]  32 secret[32]      64 u64 boot time (ms)
 *     72..4091 zero                  4092 u32 CRC-32 of bytes 0..4091
 *
 * Not yet attached to the guest: no broker inside iOS reads it, so the VM's
 * disks are unchanged. A later stage exposes this block to the root broker. */
object SystemGpuChannel {
    const val SIZE=4096
    private const val VERSION=1
    data class Descriptor(val port:Int,val run:ByteArray,val secret:ByteArray,val bootTime:Long) {
        init { require(port in 1..65535 && run.size==16 && secret.size==32) }
    }

    fun encode(d:Descriptor):ByteArray {
        val b=ByteBuffer.allocate(SIZE).order(ByteOrder.LITTLE_ENDIAN)
        b.put("EGC1".toByteArray(Charsets.US_ASCII)).putInt(VERSION).putInt(d.port).putInt(0).put(d.run).put(d.secret).putLong(d.bootTime)
        val bytes=b.array()
        val crc=CRC32().apply { update(bytes,0,SIZE-4) }.value.toInt()
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(SIZE-4,crc)
        return bytes
    }

    fun decode(bytes:ByteArray):Descriptor {
        require(bytes.size==SIZE) { "Channel block must be $SIZE bytes" }
        val b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        require(String(bytes,0,4,Charsets.US_ASCII)=="EGC1") { "Not a system GPU channel block" }
        require(b.getInt(4)==VERSION && b.getInt(12)==0) { "Unsupported channel version/flags" }
        val crc=CRC32().apply { update(bytes,0,SIZE-4) }.value.toInt()
        require(b.getInt(SIZE-4)==crc) { "Channel CRC mismatch" }
        for(i in 72 until SIZE-4)require(bytes[i]==0.toByte()) { "Reserved channel bytes must be zero" }
        return Descriptor(b.getInt(8),bytes.copyOfRange(16,32),bytes.copyOfRange(32,64),b.getLong(64))
    }

    fun file(privateDirectory:File)=File(privateDirectory,"system-gpu/channel.img")

    /** Atomic, owner-only. The temporary file never holds a partial secret visible to others. */
    fun write(privateDirectory:File,d:Descriptor) {
        val target=file(privateDirectory).apply { parentFile!!.mkdirs() }
        val part=File(target.path+".part")
        val bytes=encode(d)
        try {
            part.delete()
            Files.createFile(part.toPath(),PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")))
            RandomAccessFile(part,"rw").use { it.write(bytes);it.fd.sync() }
            Files.move(part.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE)
        } finally { bytes.fill(0);part.delete() }
    }

    /** Overwrites before deleting: the secret must not outlive the boot. */
    fun wipe(privateDirectory:File) {
        val target=file(privateDirectory)
        if(target.isFile)runCatching { RandomAccessFile(target,"rw").use { it.write(ByteArray(SIZE));it.fd.sync() } }
        target.delete()
    }
}

/** A failure of the experimental host during a boot turns it off for later
 * boots until the user re-enables it; the VM itself keeps running. */
class SystemGpuLatch(private val file:File) {
    val tripped get()=file.isFile
    val reason get()=runCatching { file.readText().trim() }.getOrDefault("").ifEmpty { "неизвестная ошибка" }
    fun trip(reason:String) { runCatching { file.parentFile?.mkdirs();file.writeText(reason.take(512)) } }
    fun reset() { file.delete() }
}
