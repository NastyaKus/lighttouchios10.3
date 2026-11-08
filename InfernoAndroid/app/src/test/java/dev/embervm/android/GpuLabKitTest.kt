// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import dev.embervm.android.restore.*
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class GpuLabKitTest {
    @get:Rule val tmp=TemporaryFolder()
    private fun source():VmConfig {
        val c=VmConfig(tmp.newFolder())
        GpuLabKit.sources(c).forEach { (name,file) -> file.parentFile!!.mkdirs();file.writeText("synthetic-$name") }
        File(c.dataDirectory,"root.qcow2").writeText("user disk, never attach or copy")
        File(c.dataDirectory,"nvram").writeText("user NVRAM, never copy")
        File(c.dataDirectory,"firmware").writeText("user firmware, never copy")
        return c
    }
    private fun prepare(c:VmConfig,base:File):GpuLabKit.Kit=GpuLabKit.prepare(c,File(base,"run-1-abcdef123456"),{_,out -> out.writeText("synthetic GPU ramdisk")})
    private fun hashes(c:VmConfig)=c.root.walkTopDown().filter { it.isFile }.associate { it.relativeTo(c.root).path to it.readBytes().contentHashCode() }
    private fun rejects(action:()->Unit){try { action();fail("Expected rejection") } catch(_:IllegalArgumentException){} catch(_:IllegalStateException){} }
    @Test fun sourceFilesArePreservedAndOnlyFreshScratchDisksAreAttached() {
        val c=source();val before=hashes(c);val base=tmp.newFolder();val kit=prepare(c,base)
        try {
            assertEquals(before,hashes(c));assertFalse(File(kit.config.dataDirectory,"root.qcow2").exists())
            assertEquals(32L shl 30,File(kit.config.dataDirectory,"root").length())
            RandomAccessFile(File(kit.config.dataDirectory,"nvram"),"r").use { assertEquals(0,it.readInt()) }
            val channel=kit.config.transferImage;channel.writeText("test-only transport placeholder")
            val args=GpuLabKit.arguments(kit,File("/qemu-data"),false)
            assertFalse(args.any { it.contains(c.root.path) })
            assertTrue(args.any { it.contains("nsid=8,nstype=2") })
            assertTrue(args.any { it.contains("boot-mode=enter_recovery") })
            assertTrue(args.any { it.contains("hold-reset,value=on") })
            assertTrue("-initrd" in args);assertFalse("-netdev" in args)
            assertEquals("2G",args[args.indexOf("-m")+1]);assertEquals("tcg,thread=multi,tb-size=64",args[args.indexOf("-accel")+1])
            assertEquals(kit.rootFingerprint,GpuLabKit.fingerprint(File(kit.config.dataDirectory,"root")))
        } finally { GpuLabKit.cleanup(base,kit.config.root) }
        assertEquals(before,hashes(c))
    }
    @Test fun refusesExistingOrNestedDestinationAndUnmarkedOrSymlinkCleanup() {
        val c=source();val base=tmp.newFolder();val kit=prepare(c,base)
        try {
            rejects { prepare(c,base) }
            rejects { GpuLabKit.prepare(c,File(c.root,"run-2-abcdef123456"),{_,_->}) }
            val unmarked=File(base,"run-3-abcdef123456").apply { mkdirs() }
            File(unmarked,"keep").writeText("untouched");rejects { GpuLabKit.cleanup(base,unmarked) };assertTrue(File(unmarked,"keep").isFile)
            val alias=File(base,"run-4-abcdef123456");Files.createSymbolicLink(alias.toPath(),c.root.toPath())
            rejects { GpuLabKit.cleanup(base,alias) };assertTrue(c.root.isDirectory)
        } finally { GpuLabKit.cleanup(base,kit.config.root) }
    }
    @Test fun detectsIncompleteChannelAndSentinelDamage() {
        val c=source();val base=tmp.newFolder();val kit=prepare(c,base)
        try {
            val f=kit.config.transferImage;f.writeBytes(ByteArray(64));assertFalse(GpuLabKit.channelReady(f))
            val header=ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN)
            header.put("EMBER_GPU_PROBE1".toByteArray());header.putInt(1);header.putInt(16*1024*1024)
            RandomAccessFile(f,"rw").use { it.setLength(16L*1024*1024);it.write(header.array()) }
            assertTrue(GpuLabKit.channelReady(f))
            RandomAccessFile(f,"rw").use { it.seek(16);it.writeInt(2) };assertFalse(GpuLabKit.channelReady(f))
            val disk=File(kit.config.dataDirectory,"root");RandomAccessFile(disk,"rw").use { it.writeInt(0) }
            assertNotEquals(kit.rootFingerprint,GpuLabKit.fingerprint(disk))
        } finally { GpuLabKit.cleanup(base,kit.config.root) }
    }
    @Test fun gpuRamdiskDisablesRestoreAndContainsOnlyOurProbeAndLibrary() {
        val stock=tmp.newFile();stock.writeBytes(javaClass.getResourceAsStream("/restore/patchboot-stock.im4p")!!.readBytes())
        val before=stock.readBytes();val out=File(tmp.root,"gpu.im4p")
        RestoreRamdisk.buildGpuProbe(stock,out,"our-probe".toByteArray(),"our-dylib".toByteArray())
        assertArrayEquals(before,stock.readBytes())
        val payload=Img4.readIM4P(out.readBytes()).data;val b=ByteBuffer.wrap(payload).order(ByteOrder.BIG_ENDIAN)
        assertEquals("our-probe",String(payload,36*512,9));assertEquals("our-dylib",String(payload,40*512,9))
        fun job(resource:String):Map<*,*> {
            val record=javaClass.getResourceAsStream(resource)!!.bufferedReader().use { it.readText().trim().toInt() }
            val start=b.getInt(record+104)*512;val size=b.getLong(record+88).toInt()
            return Plist.parse(payload.copyOfRange(start,start+size).inputStream()) as Map<*,*>
        }
        val restored=job("/restore/patchboot-record-offset.txt")
        assertEquals(true,restored["Disabled"]);assertEquals(false,restored["RunAtLoad"])
        assertEquals(listOf("/usr/bin/nvmefwupdater","--skip"),job("/restore/patchboot-storage-offset.txt")["ProgramArguments"])
    }
}
