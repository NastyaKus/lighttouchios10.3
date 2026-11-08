package dev.embervm.android.restore

import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.util.concurrent.TimeUnit

class RestoreAttemptTest {
    @Test fun staleMarkerDoesNotBlockRetryOrModifyPreparedInputs() {
        val root=Files.createTempDirectory("restore-retry").toFile()
        try {
            val stage=RestorePrep.staging(root).apply { mkdirs() }
            val marker=File(stage,"restore-running").apply { writeText("old-process") }
            val input=File(stage,"input.ipsw").apply { writeText("synthetic-input-preserved") }
            RestoreAttempt.acquire(root).use { assertEquals("old-process",marker.readText());assertEquals("synthetic-input-preserved",input.readText()) }
            RestoreAttempt.acquire(root).close()
        } finally { root.deleteRecursively() }
    }
    @Test fun liveConcurrentAttemptIsRejectedWithoutDeletingItsMarker() {
        val root=Files.createTempDirectory("restore-concurrent").toFile()
        try {
            val stage=RestorePrep.staging(root).apply { mkdirs() };val marker=File(stage,"restore-running").apply { writeText("live-process") }
            RestoreAttempt.acquire(root).use {
                try { RestoreAttempt.acquire(root).close();fail("Concurrent lease must be rejected") } catch(_:IllegalArgumentException) { }
                assertEquals("live-process",marker.readText())
            }
            RestoreAttempt.acquire(root).close()
        } finally { root.deleteRecursively() }
    }
    @Test fun operatingSystemReleasesLiveLockWhenHolderIsKilled() {
        val root=Files.createTempDirectory("restore-killed").toFile()
        val script="import fcntl,sys; f=open(sys.argv[1],'a+'); fcntl.lockf(f,fcntl.LOCK_EX); print('locked',flush=True); sys.stdin.read()"
        val holder=ProcessBuilder("python3","-u","-c",script,File(root,"restore-session.lock").path).start()
        try {
            assertEquals("locked",holder.inputStream.bufferedReader().readLine())
            try { RestoreAttempt.acquire(root).close();fail("Other process owns the lock") } catch(_:IllegalArgumentException) { }
            holder.destroyForcibly();assertTrue(holder.waitFor(5,TimeUnit.SECONDS))
            RestoreAttempt.acquire(root).close()
        } finally { holder.destroyForcibly();root.deleteRecursively() }
    }
    @Test fun archivesLogsBeforeRetryWithoutCopyingAppleInputsOrChangingSource() {
        val root=Files.createTempDirectory("restore-archive").toFile()
        try {
            val stage=RestorePrep.staging(root).apply { mkdirs() }
            val guest=File(stage,"guest-console.log").apply { writeText("old guest log") }
            File(stage,"input.ipsw").writeText("synthetic firmware")
            File(stage,"AppleSEPROM-Cebu-B1").writeText("synthetic ROM")
            File(root,"restore-status.plist").writeText("old status")
            File(root,"restore.log").writeText("old host log")
            RestoreAttempt.acquire(root).use {
                val archive=RestoreAttempt.archivePrevious(root,"123-456")
                assertEquals("old guest log",File(archive,"guest-console.log").readText())
                assertEquals("old status",File(archive,"status.plist").readText())
                assertFalse(File(archive,"input.ipsw").exists());assertFalse(File(archive,"AppleSEPROM-Cebu-B1").exists())
                assertEquals("old guest log",guest.readText())
            }
        } finally { root.deleteRecursively() }
    }
    @Test fun missingInputIsRejectedBeforeDiskReset() {
        val root=Files.createTempDirectory("restore-preflight").toFile()
        try {
            val stage=RestorePrep.staging(root).apply { mkdirs() }
            File(stage,"prepared.plist").writeText("prepared")
            val disk=File(stage,"InfernoData/root").apply { parentFile!!.mkdirs();writeText("previous-attempt-disk") }
            try { RestoreAttempt.validatePrepared(root);fail() } catch(_:IllegalArgumentException) { }
            assertEquals("previous-attempt-disk",disk.readText())
        } finally { root.deleteRecursively() }
    }
}
