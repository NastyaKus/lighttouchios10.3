// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import dev.embervm.android.restore.PlistXml
import dev.embervm.android.restore.RestorePrep
import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SystemGpuProfileTest {
    @get:Rule val tmp=TemporaryFolder()
    private val id="system-"+"a".repeat(32)
    private fun source():VmConfig {
        val config=VmConfig(tmp.newFolder())
        for(file in GpuLabKit.sources(config).values) { file.parentFile!!.mkdirs();file.writeText("synthetic input") }
        File(config.dataDirectory,"Restore/BuildManifest.plist").writeBytes(PlistXml.encode(mapOf("ProductBuildVersion" to SystemGpuProfile.BUILD)))
        for((name,size) in RestorePrep.diskSizes)RandomAccessFile(File(config.dataDirectory,name),"rw").use {
            it.setLength(if(name=="root")3L shl 20 else size)
            it.writeLong(0x0102030405060708);it.seek(it.length()-8);it.writeLong(0x0807060504030201)
        }
        return config
    }
    private fun hash(root:File)=root.walkTopDown().filter { it.isFile }.associate { file ->
        file.relativeTo(root).path to MessageDigest.getInstance("SHA-256").digest(file.readBytes()).toList()
    }
    @Test fun independentProfilePreservesAllStateAndNeverClaimsAcceleration() {
        val source=source();val before=hash(source.root);val private=tmp.newFolder()
        val result=SystemGpuProfile.create(source,private,id)
        assertTrue(result.files>=15);assertTrue(result.logicalBytes>8L shl 20)
        assertEquals(before,hash(source.root))
        for(name in RestorePrep.diskSizes.keys)assertArrayEquals(File(source.dataDirectory,name).readBytes(),File(result.config.dataDirectory,name).readBytes())
        val receipt=Plist.parse(File(result.config.root,"copied.plist")) as Map<*,*>
        assertEquals("COPIED",receipt["State"]);assertEquals("SOFTWARE",receipt["GuestGraphicsMode"])
        assertEquals(result.files,(receipt["FilesSHA256"] as Map<*,*>).size)
        File(result.config.dataDirectory,"nvram").writeText("independent changes")
        assertEquals(before,hash(source.root));assertFalse(File(SystemGpuProfile.directory(private),"$id.part").exists())
    }
    @Test fun cancellationDoesNotPublishPartialOrChangeBase() {
        val source=source();val before=hash(source.root);val private=tmp.newFolder();var cancel=false
        val outcome=runCatching { SystemGpuProfile.create(source,private,id,cancelled={cancel},note={cancel=true}) }
        assertTrue(outcome.exceptionOrNull() is ImportCancelled)
        assertFalse(File(SystemGpuProfile.directory(private),id).exists());assertFalse(File(SystemGpuProfile.directory(private),"$id.part").exists())
        assertEquals(before,hash(source.root));VmLease.acquire(private).close()
    }
    @Test fun changedSourceCannotBePublished() {
        val source=source();val private=tmp.newFolder();var changed=false
        val outcome=runCatching { SystemGpuProfile.create(source,private,id,note={ progress ->
            if(!changed){changed=true;RandomAccessFile(File(source.root,progress.file),"rw").use { it.setLength(1) }}
        }) }
        assertTrue(outcome.isFailure);assertFalse(File(SystemGpuProfile.directory(private),id).exists())
        assertFalse(File(SystemGpuProfile.directory(private),"$id.part").exists())
    }
    @Test fun refusesBusyUnsupportedMissingOrAliasedInputs() {
        val source=source();val private=tmp.newFolder()
        VmLease.acquire(private).use { assertTrue(runCatching { SystemGpuProfile.create(source,private,id) }.isFailure) }
        val manifest=File(source.dataDirectory,"Restore/BuildManifest.plist")
        manifest.writeBytes(PlistXml.encode(mapOf("ProductBuildVersion" to "20H392")))
        assertTrue(runCatching { SystemGpuProfile.create(source,private,id) }.isFailure)
        manifest.writeBytes(PlistXml.encode(mapOf("ProductBuildVersion" to SystemGpuProfile.BUILD)))
        File(source.dataDirectory,"sep_ssc").delete();assertTrue(runCatching { SystemGpuProfile.create(source,private,id) }.isFailure)
        assertFalse(File(SystemGpuProfile.directory(private),id).exists())
        assertTrue(runCatching { SystemGpuProfile.create(source,source.root,id) }.isFailure)
    }
    @Test fun refusesSymlinkStateAndExistingProfile() {
        val source=source();val private=tmp.newFolder();val state=File(source.dataDirectory,"nvram")
        val outside=tmp.newFile().apply { writeBytes(state.readBytes()) };state.delete();Files.createSymbolicLink(state.toPath(),outside.toPath())
        assertTrue(runCatching { SystemGpuProfile.create(source,private,id) }.isFailure)
        state.delete();state.writeBytes(outside.readBytes())
        val result=SystemGpuProfile.create(source,private,id);val before=hash(result.config.root)
        assertTrue(runCatching { SystemGpuProfile.create(source,private,id) }.isFailure)
        assertEquals(before,hash(result.config.root))
    }
}
