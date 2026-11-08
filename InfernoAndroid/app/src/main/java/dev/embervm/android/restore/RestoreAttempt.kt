// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android.restore

import java.io.Closeable
import java.io.File
import java.io.RandomAccessFile
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException

/** Process-owned lock: an orphaned marker after SIGKILL is not a live restore. */
class RestoreAttempt private constructor(private val file:RandomAccessFile,private val lock:FileLock):Closeable {
    override fun close() { try { lock.release() } finally { file.close() } }
    companion object {
        fun acquire(root:File):RestoreAttempt {
            val handle=RandomAccessFile(File(root,"restore-session.lock"),"rw")
            try {
                val lock=try { handle.channel.tryLock() } catch(_:OverlappingFileLockException) { null }
                require(lock!=null) { "Восстановление уже выполняется" }
                return RestoreAttempt(handle,lock)
            } catch(e:Exception) { handle.close();throw e }
        }
        /** Called only under the live lock, before resetting the guest disks. */
        fun archivePrevious(root:File,attemptID:String):File {
            require(Regex("[0-9-]+").matches(attemptID))
            val archive=File(root,"restore-attempts/$attemptID").apply { check(mkdirs()) }
            val stage=RestorePrep.staging(root)
            val paths=listOf(
                File(root,"restore-status.plist") to "status.plist",
                File(root,"restore.log") to "host.log",
                File(root,"restore-error.log") to "restore-error.log",
                File(stage,"restored.plist") to "restored-checkpoint.plist",
                File(stage,"guest-console.log") to "guest-console.log",
                File(stage,"logs/engine.log") to "engine.log",
                File(root,"vm-lifecycle.log") to "vm-lifecycle.log",
                File(root,"java-crash.log") to "java-crash.log"
            )
            for((source,name) in paths)if(source.isFile) {
                // Diagnostics only: do not copy Apple inputs, firmware, keys or disks.
                RandomAccessFile(source,"r").use { input ->
                    input.seek((input.length()-2*1024*1024).coerceAtLeast(0))
                    File(archive,name).outputStream().use { out ->
                        val buffer=ByteArray(65536)
                        while(true) { val n=input.read(buffer);if(n<0)break;out.write(buffer,0,n) }
                    }
                }
            }
            // Bound retained diagnostic storage without touching any guest files.
            File(root,"restore-attempts").listFiles()?.filter { it.isDirectory && it!=archive }
                ?.sortedByDescending { it.name }?.drop(2)?.forEach { it.deleteRecursively() }
            return archive
        }
        fun validatePrepared(root:File) {
            val stage=RestorePrep.staging(root)
            require(File(stage,"prepared.plist").isFile) { "Сначала подготовьте IPSW" }
            require(File(stage,"input.ipsw").length()>0) { "Нет рабочей копии IPSW. Подготовьте набор заново." }
            require(File(stage,"restore-ramdisk.im4p").length()>0) { "Нет подготовленного ramdisk" }
            require(File(stage,"AppleSEPROM-Cebu-B1").length()>0) { "Нет подготовленного SEP ROM" }
            require(dev.embervm.android.VmConfig(stage).missing().isEmpty()) { "Подготовленный набор неполон: ${dev.embervm.android.VmConfig(stage).missing().joinToString()}" }
            require(!File(root,"restore-activation.plist").exists()) { "Сначала верните предыдущий набор" }
        }
    }
}
