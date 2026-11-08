// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android.restore

import org.junit.Assert.*
import org.junit.Test
import dev.embervm.android.Plist
import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class RestoreCheckpointTest {
    private val replacementDaemon="updated-daemon-test".toByteArray()
    private fun fixture():File {
        val root=Files.createTempDirectory("restore-checkpoint").toFile()
        val stage=RestorePrep.staging(root).apply { mkdirs() }
        File(stage,"prepared.plist").writeBytes(PlistXml.encode(mapOf("Build" to "18A5351d","GuestTools" to true,"State" to "PREPARED")))
        for((name,size) in RestorePrep.diskSizes) {
            val disk=File(stage,"InfernoData/$name").apply { parentFile!!.mkdirs() }
            RandomAccessFile(disk,"rw").use { it.setLength(size);it.write("synthetic-$name".toByteArray()) }
        }
        val injected=File(stage,"restore-ramdisk.im4p")
        injected.writeBytes(javaClass.getResourceAsStream("/restore/patchboot-stock.im4p")!!.readBytes())
        val path="Firmware/restore.dmg"
        File(stage,"InfernoData/Restore/$path").apply { parentFile!!.mkdirs();writeText("synthetic stock ramdisk") }
        val identity=mapOf("Info" to mapOf("DeviceClass" to "n104ap","Variant" to "Research Developer Erase"),
            "Manifest" to mapOf("RestoreRamDisk" to mapOf("Info" to mapOf("Path" to path))))
        ZipOutputStream(File(stage,"input.ipsw").outputStream()).use {
            it.putNextEntry(ZipEntry("BuildManifest.plist"));it.write(PlistXml.encode(mapOf("ProductBuildVersion" to "18A5351d","BuildIdentities" to listOf(identity))));it.closeEntry()
        }
        return root
    }
    private fun diskHashes(root:File):Map<String,Pair<Long,List<Byte>>> = RestorePrep.diskSizes.mapValues { (name,_)->
        val file=File(RestorePrep.staging(root),"InfernoData/$name")
        val head=RandomAccessFile(file,"r").use { val b=ByteArray(minOf(65536L,it.length()).toInt());it.readFully(b);b }
        file.length() to MessageDigest.getInstance("SHA-256").digest(head).toList()
    }
    private fun legacy(root:File) {
        File(root,"restore.log").writeText("USB RESET on existing socket\nrestored: Status 0 подтверждён\n")
        File(RestorePrep.staging(root),"guest-console.log").writeText("CHECKPOINT FINISHED-ENGINES:(SUCCESS)\nrestore-outcome = success\nCHECKPOINT FINAL-MONITOR:(SUCCESS) ... reboot\n")
    }
    @Test fun patchBootRequiresProofAndPreservesAllNineDisks() {
        val root=fixture()
        try {
            val before=diskHashes(root);assertFalse(RestoreCheckpoint.canPatch(root))
            try { RestoreCheckpoint.enterPatchBoot(root);fail() } catch(_:IllegalArgumentException) { }
            assertEquals(before,diskHashes(root))
            RestoreCheckpoint.record(root);assertTrue(RestoreCheckpoint.canPatch(root))
            RestoreCheckpoint.enterPatchBoot(root)
            assertEquals(before,diskHashes(root));assertTrue(RestoreCheckpoint.canPatch(root))
        } finally { root.deleteRecursively() }
    }
    @Test fun adoptsOnlyLatestLegacySuccessAndKeepsReceiptWhenOldLogsAreReplaced() {
        val root=fixture()
        try {
            legacy(root);val before=diskHashes(root);assertTrue(RestoreCheckpoint.canPatch(root))
            File(root,"restore.log").appendText("USB RESET on existing socket\nnew failed attempt\n")
            assertFalse(RestoreCheckpoint.canPatch(root))
            legacy(root);RestoreCheckpoint.enterPatchBoot(root)
            File(root,"restore.log").writeText("new patch-only run")
            File(RestorePrep.staging(root),"guest-console.log").writeText("new boot")
            assertTrue(RestoreCheckpoint.canPatch(root));assertEquals(before,diskHashes(root))
            val p=Plist.parse(File(RestorePrep.staging(root),"restored.plist")) as Map<*,*>
            assertEquals("legacy-0.2.5-logs",p["Source"])
        } finally { root.deleteRecursively() }
    }
    @Test fun incompleteOrNonzeroLegacyResultCannotAuthorizePatches() {
        val root=fixture()
        try {
            legacy(root);File(root,"restore.log").writeText("USB RESET on existing socket\nrestored: Status 1 подтверждён\n")
            assertFalse(RestoreCheckpoint.canPatch(root))
            legacy(root);File(RestorePrep.staging(root),"guest-console.log").writeText("ASR RESTORE PROGRESS: 100%\n*** PATCHER: patching cache\n")
            assertFalse(RestoreCheckpoint.canPatch(root))
        } finally { root.deleteRecursively() }
    }
    @Test fun receiptBindsPreparationAndCannotFallBackToOldLogsWhenCorrupted() {
        val root=fixture()
        try {
            legacy(root);RestoreCheckpoint.record(root)
            val marker=File(RestorePrep.staging(root),"prepared.plist")
            marker.writeBytes(PlistXml.encode(mapOf("Build" to "18A5351d","GuestTools" to true,"State" to "different preparation")))
            assertFalse(RestoreCheckpoint.canPatch(root))
            File(RestorePrep.staging(root),"restored.plist").writeText("truncated")
            assertFalse(RestoreCheckpoint.canPatch(root))
        } finally { root.deleteRecursively() }
    }
    @Test fun fullRetryInvalidatesProofBeforeClearingDisksEvenIfLegacyLogsRemain() {
        val root=fixture()
        try {
            legacy(root);RestoreCheckpoint.record(root);assertTrue(RestoreCheckpoint.canPatch(root))
            RestoreCheckpoint.resetDisksForRestore(root)
            assertFalse(RestoreCheckpoint.canPatch(root))
            for((name,size) in RestorePrep.diskSizes) {
                val file=File(RestorePrep.staging(root),"InfernoData/$name");assertEquals(size,file.length())
                RandomAccessFile(file,"r").use { assertEquals(0,it.readInt()) }
            }
            // Even a later nonzero header cannot revive stale legacy success.
            RandomAccessFile(File(RestorePrep.staging(root),"InfernoData/root"),"rw").use { it.writeInt(123) }
            assertFalse(RestoreCheckpoint.canPatch(root))
        } finally { root.deleteRecursively() }
    }
    @Test fun emptyOrResizedDiskAndMissingGuestToolsRejectPatchBoot() {
        val root=fixture()
        try {
            RestoreCheckpoint.record(root)
            val disk=File(RestorePrep.staging(root),"InfernoData/root")
            RandomAccessFile(disk,"rw").use { it.write(ByteArray(65536)) }
            assertFalse(RestoreCheckpoint.canPatch(root))
            RandomAccessFile(disk,"rw").use { it.writeInt(1);it.setLength(8192) }
            assertFalse(RestoreCheckpoint.canPatch(root))
            File(RestorePrep.staging(root),"prepared.plist").writeBytes(PlistXml.encode(mapOf("Build" to "18A5351d","GuestTools" to false)))
            assertFalse(RestoreCheckpoint.canPatch(root))
        } finally { root.deleteRecursively() }
    }
    @Test fun fullRestoreUsesStockRamdiskAndPatchBootUsesExistingInjectedRamdisk() {
        val root=fixture()
        try {
            assertEquals("synthetic stock ramdisk",RestoreCheckpoint.ramdisk(root,false).readText())
            val injected=File(RestorePrep.staging(root),"restore-ramdisk.im4p");val before=injected.readBytes()
            val patchBoot=RestoreCheckpoint.ramdisk(root,true,replacementDaemon)
            assertEquals("patch-ramdisk.im4p",patchBoot.name);assertArrayEquals(before,injected.readBytes())
            assertFalse(before.contentEquals(patchBoot.readBytes()))
            RestoreCheckpoint.ramdisk(root,false).delete()
            try { RestoreCheckpoint.ramdisk(root,false);fail() } catch(_:IllegalArgumentException) { }
        } finally { root.deleteRecursively() }
    }
    @Test fun guestPanicInvalidatesEvenOtherwiseCompletePatcherEvidence() {
        val evidence=PatchEvidence()
        evidence.accept("panic log is empty");assertNull(evidence.panic)
        evidence.accept("*** PATCHER: the patcher exited with 0")
        evidence.accept("*** PATCHER: five services switched off")
        evidence.accept("*** PATCHER: patch plan verified 18A5351d-v031-software all=45")
        evidence.accept("*** PATCHER: cleanup complete")
        evidence.accept("*** PATCHER: done, the machine can be reset now")
        assertTrue(evidence.successful)
        evidence.accept("panic(cpu 2 caller 0x1): \"Halt/Restart Timed Out\"")
        assertFalse(evidence.successful);assertTrue(evidence.panic!!.contains("Halt/Restart Timed Out"))
    }
    @Test fun patchRamdiskDisablesRestoreMonitorUpdatesDaemonAndPreservesFsPatcher() {
        val root=fixture()
        try {
            val stage=RestorePrep.staging(root);val base=File(stage,"restore-ramdisk.im4p")
            val injected=File(stage,"with-tools.im4p");RestoreRamdisk.build(base,injected,"daemon-test".toByteArray(),"patcher-test".toByteArray())
            val before=injected.readBytes();val patched=File(stage,"patch-only.im4p")
            RestoreRamdisk.buildPatchBoot(injected,patched,replacementDaemon)
            assertArrayEquals(before,injected.readBytes());assertEquals(injected.length(),patched.length())
            val payload=Img4.readIM4P(patched.readBytes()).data
            assertEquals(String(replacementDaemon),String(payload,36*512,replacementDaemon.size));assertEquals("patcher-test",String(payload,40*512,12))
            val record=javaClass.getResourceAsStream("/restore/patchboot-record-offset.txt")!!.bufferedReader().use { it.readText().trim().toInt() }
            val b=java.nio.ByteBuffer.wrap(payload).order(java.nio.ByteOrder.BIG_ENDIAN)
            val start=b.getInt(record+104)*512;val size=b.getLong(record+88).toInt()
            val job=Plist.parse(payload.copyOfRange(start,start+size).inputStream()) as Map<*,*>
            assertEquals(true,job["Disabled"]);assertEquals(false,job["RunAtLoad"])
            assertEquals("com.apple.restored_external",job["Label"])
            assertEquals(0,payload[record+41].toInt());assertEquals(0x81a4,b.getShort(record+42).toInt() and 65535)
            val storageRecord=javaClass.getResourceAsStream("/restore/patchboot-storage-offset.txt")!!.bufferedReader().use { it.readText().trim().toInt() }
            val storageStart=b.getInt(storageRecord+104)*512;val storageSize=b.getLong(storageRecord+88).toInt()
            val storage=Plist.parse(payload.copyOfRange(storageStart,storageStart+storageSize).inputStream()) as Map<*,*>
            assertEquals(listOf("/usr/bin/nvmefwupdater","--skip"),storage["ProgramArguments"])
            assertEquals(true,storage["RunAtLoad"]);assertEquals(5L,storage["ThrottleInterval"])
            assertEquals(false,(storage["KeepAlive"] as Map<*,*>)["SuccessfulExit"])
            assertEquals(0,payload[storageRecord+41].toInt())
        } finally { root.deleteRecursively() }
    }
    @Test fun missingStorageJobCannotPublishOrOverwritePatchRamdisk() {
        val root=fixture()
        try {
            val stage=RestorePrep.staging(root);val injected=File(stage,"restore-ramdisk.im4p")
            val image=Img4.readIM4P(injected.readBytes());val payload=image.data.copyOf()
            val record=javaClass.getResourceAsStream("/restore/patchboot-storage-offset.txt")!!.bufferedReader().use { it.readText().trim().toInt() }
            val name="com.apple.KeyMaker.plist";val key=record-(8+name.length*2)
            payload[key+8+"com.apple.".length*2+1]='X'.code.toByte()
            injected.writeBytes(Img4.makeIM4P("rdsk","synthetic",payload))
            val destination=File(stage,"patch-ramdisk.im4p").apply { writeText("previous output preserved") }
            try { RestoreRamdisk.buildPatchBoot(injected,destination,replacementDaemon);fail() } catch(_:IllegalStateException) { }
            assertEquals("previous output preserved",destination.readText());assertFalse(File(destination.path+".part").exists())
        } finally { root.deleteRecursively() }
    }
    @Test fun daemonErrorCannotBecomeSuccessEvenWithAllOtherMarkers() {
        val evidence=PatchEvidence()
        evidence.accept("*** PATCHER: the patcher exited with 0")
        evidence.accept("*** PATCHER: five services switched off")
        evidence.accept("*** PATCHER: ERROR: umount failed")
        evidence.accept("*** PATCHER: patch plan verified 18A5351d-v031-software all=45")
        evidence.accept("*** PATCHER: cleanup complete")
        evidence.accept("*** PATCHER: done, the machine can be reset now")
        assertFalse(evidence.successful);assertNotNull(evidence.error)
    }
    @Test fun patchRamdiskRequiresNewDaemonBeforePublishing() {
        val root=fixture()
        try {
            val output=File(RestorePrep.staging(root),"patch-ramdisk.im4p").apply { writeText("previous") }
            try { RestoreCheckpoint.ramdisk(root,true);fail() } catch(_:IllegalArgumentException) { }
            assertEquals("previous",output.readText())
        } finally { root.deleteRecursively() }
    }
    @Test fun volumeWaitLogIsNotPatcherSuccessAndReadyVolumesEndThatWait() {
        val evidence=PatchEvidence()
        evidence.accept("virtual bool AppleEmbeddedNVMeController::InitializeController():FW update not complete, create dummy block device")
        evidence.accept("*** PATCHER: waiting for the restore to finish")
        assertTrue(evidence.firmwareGate);assertTrue(evidence.waitingForVolumes)
        assertFalse(evidence.volumesReady);assertFalse(evidence.successful)
        evidence.accept("Skipped firmware update successfully")
        assertTrue(evidence.storageInitialized);assertFalse(evidence.firmwareGate)
        evidence.accept("*** PATCHER: the data volume is here -- the restore is done writing")
        assertTrue(evidence.volumesReady);assertFalse(evidence.waitingForVolumes);assertFalse(evidence.successful)
    }
    @Test fun volumeWaitTimesOutAtThreeMinutesButDoesNotLimitCacheProcessing() {
        val evidence=PatchEvidence();evidence.accept("*** PATCHER: waiting for the restore to finish")
        assertFalse(evidence.volumesTimedOut(1000));assertFalse(evidence.volumesTimedOut(180999))
        assertTrue(evidence.volumesTimedOut(181000))
        evidence.accept("*** PATCHER: the data volume is here -- the restore is done writing")
        assertFalse(evidence.volumesTimedOut(3600000))
    }
    @Test fun nonzeroChildMustNotAbortWhileGuestIsStillUnmounting() {
        val evidence=PatchEvidence()
        evidence.accept("*** PATCHER: the patcher exited with 1")
        assertFalse(evidence.failureReady)
        evidence.accept("*** PATCHER: unmounting /mnt9")
        assertFalse(evidence.failureReady);assertFalse(evidence.successful)
        evidence.accept("*** PATCHER: unmount /mnt9 says 0 errno=0")
        assertFalse(evidence.failureReady)
        evidence.accept("*** PATCHER: cleanup complete")
        assertTrue(evidence.failureReady);assertFalse(evidence.successful)
    }
    @Test fun verifiedExistingBytesNeedServicesAndCleanCompletionWithoutForgedExitZero() {
        val e=PatchEvidence()
        e.accept("*** PATCHER: patch plan verified 18A5351d-v031-software all=44")
        assertFalse(e.planVerified)
        e.accept("*** PATCHER: patch plan verified other-build all=45")
        assertFalse(e.planVerified)
        e.accept("*** PATCHER: patch plan verified 18A5351d-v031-software all=45")
        assertTrue(e.planVerified);assertNull(e.exitCode);assertFalse(e.successful)
        e.accept("*** PATCHER: five services switched off")
        e.accept("*** PATCHER: done, the machine can be reset now")
        assertFalse(e.successful)
        e.accept("*** PATCHER: cleanup complete")
        assertTrue(e.successful)
        e.accept("*** PATCHER: ERROR: failed")
        assertFalse(e.successful)
    }
    @Test fun exitZeroAndDoneDoNotReplaceReadBackVerification() {
        val e=PatchEvidence()
        e.accept("*** PATCHER: the patcher exited with 0")
        e.accept("*** PATCHER: five services switched off")
        e.accept("*** PATCHER: cleanup complete")
        e.accept("*** PATCHER: done, the machine can be reset now")
        assertFalse(e.successful)
    }

    @Test fun systemOnlyPatchBootDoesNotWaitForDataVolumeNode() {
        val e=PatchEvidence()
        e.accept("*** PATCHER: waiting for the restore to finish")
        assertFalse(e.volumesTimedOut(1000))
        e.accept("*** PATCHER: the system volume is here -- patch-only boot")
        assertTrue(e.volumesReady);assertFalse(e.waitingForVolumes)
        assertFalse(e.volumesTimedOut(999999))
        assertFalse(e.successful)
    }

}
