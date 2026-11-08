// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android.restore

import java.io.File
import dev.embervm.android.Plist
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** A successful restored result is NOT a completed/patched installation. */
object RestoreCheckpoint {
    private const val BUILD="18A5351d"
    private fun receipt(root:File)=File(RestorePrep.staging(root),"restored.plist")
    private fun invalidated(root:File)=File(RestorePrep.staging(root),"restore-proof-invalidated")
    private fun prepared(root:File):Map<*,*> {
        val file=File(RestorePrep.staging(root),"prepared.plist")
        require(file.length() in 1..131072) { "Нет корректного маркера подготовки" }
        val p=Plist.parse(file) as? Map<*,*> ?: error("Invalid preparation marker")
        require(p["Build"]==BUILD) { "Нужна сборка $BUILD" };return p
    }
    private fun preparationHash(root:File):String = MessageDigest.getInstance("SHA-256")
        .digest(File(RestorePrep.staging(root),"prepared.plist").readBytes()).joinToString("") { "%02x".format(it) }
    private fun checkWrittenDisks(root:File) {
        val data=File(RestorePrep.staging(root),"InfernoData")
        require(RestorePrep.diskSizes.all { (name,size)->File(data,name).isFile && File(data,name).length()==size }) { "Размеры рабочих дисков изменились" }
        // Reject a freshly emptied sparse disk. This is only a basic guard;
        // the receipt/logs prove restored completed, the guest validates APFS.
        val bytes=ByteArray(65536)
        RandomAccessFile(File(data,"root"),"r").use { it.readFully(bytes) }
        require(bytes.any { it!=0.toByte() }) { "Диск пуст: подтверждения записи iOS нет" }
    }
    private fun hasReceipt(root:File):Boolean = runCatching {
        val file=receipt(root);require(file.length() in 1..131072)
        val p=Plist.parse(file) as? Map<*,*> ?: return@runCatching false
        p["Build"]==BUILD && p["State"]=="RESTORED_NEEDS_PATCHES" &&
            (p["RestoredStatus"] as? Number)?.toInt()==0 && p["PreparedSHA256"]==preparationHash(root)
    }.getOrDefault(false)
    private fun tail(file:File):String {
        if(!file.isFile)return ""
        return RandomAccessFile(file,"r").use {
            it.seek((it.length()-4*1024*1024).coerceAtLeast(0));val b=ByteArray((it.length()-it.filePointer).toInt())
            it.readFully(b);b.toString(Charsets.UTF_8)
        }
    }
    /** Upgrade path for 0.2.5, which didn't yet save a successful-restore receipt. */
    private fun hasLegacyProof(root:File):Boolean {
        if(invalidated(root).exists() || receipt(root).exists())return false
        val host=tail(File(root,"restore.log"));val start=host.lastIndexOf("USB RESET on existing socket")
        if(start<0 || !host.substring(start).lineSequence().any { it.trim()=="restored: Status 0 подтверждён" })return false
        val guest=tail(File(RestorePrep.staging(root),"guest-console.log"))
        return guest.contains("CHECKPOINT FINISHED-ENGINES:(SUCCESS)") &&
            guest.contains("CHECKPOINT FINAL-MONITOR:(SUCCESS)") &&
            guest.lineSequence().any { it.trim()=="restore-outcome = success" }
    }
    fun canPatch(root:File):Boolean=runCatching {
        require(prepared(root)["GuestTools"]==true);checkWrittenDisks(root)
        hasReceipt(root) || hasLegacyProof(root)
    }.getOrDefault(false)
    /** Metadata only, for diagnosing an unavailable patch-only button. */
    fun diagnostics(root:File):String=buildString {
        appendLine("Patch-only eligible=${canPatch(root)}")
        appendLine("Receipt present=${receipt(root).isFile} valid=${hasReceipt(root)}")
        appendLine("Legacy proof=${hasLegacyProof(root)} invalidated=${invalidated(root).exists()}")
        appendLine("Prepared=${runCatching { val p=prepared(root);"build=${p["Build"]} guestTools=${p["GuestTools"]}" }.getOrElse { it.message ?: "unavailable" }}")
        appendLine("Disk guard=${runCatching { checkWrittenDisks(root);"ok" }.getOrElse { it.message ?: "unavailable" }}")
        for((name,size) in RestorePrep.diskSizes) {
            val file=File(RestorePrep.staging(root),"InfernoData/$name")
            appendLine("$name exists=${file.isFile} bytes=${file.length()} expected=$size")
        }
    }
    /** Under the session lease, BEFORE logs are archived/truncated. No disk writes. */
    fun enterPatchBoot(root:File) {
        require(canPatch(root)) { "Нет подтверждённого восстановления для патчей. Сохраните диагностику." }
        if(!hasReceipt(root))record(root,"legacy-0.2.5-logs")
    }
    /** Called only after restored Status 0/ACK AND a clean host engine stop. */
    fun record(root:File,source:String="restored-status-0") {
        prepared(root);checkWrittenDisks(root)
        val target=receipt(root);val part=File(target.path+".part")
        try {
            val bytes=PlistXml.encode(mapOf("Build" to BUILD,"State" to "RESTORED_NEEDS_PATCHES",
                "RestoredStatus" to 0,"PreparedSHA256" to preparationHash(root),"Source" to source,
                "RecordedAt" to System.currentTimeMillis()))
            part.outputStream().use { it.write(bytes);it.fd.sync() }
            Files.move(part.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING)
            invalidated(root).delete()
        } finally { part.delete() }
    }
    /** Full retry is destructive to staged disks; patch-only boot must NEVER call it. */
    fun resetDisksForRestore(root:File) {
        // Invalidate legacy evidence before touching even the first disk. A crash
        // during reset must not make old successful logs authorize patch-only.
        invalidated(root).writeText("full restore started\n")
        require(!receipt(root).exists() || receipt(root).delete()) { "Не удалось сбросить результат предыдущего восстановления" }
        for((name,size) in RestorePrep.diskSizes) {
            RandomAccessFile(File(RestorePrep.staging(root),"InfernoData/$name"),"rw").use { it.setLength(0);it.setLength(size) }
        }
    }
    fun ramdisk(root:File,patchOnly:Boolean,daemon:ByteArray?=null):File {
        val stage=RestorePrep.staging(root)
        val file=if(patchOnly) {
            val injected=File(stage,"restore-ramdisk.im4p")
            require(injected.isFile && injected.length()>0) { "Нет ramdisk с программами патчера" }
            File(stage,"patch-ramdisk.im4p").also { RestoreRamdisk.buildPatchBoot(injected,it,requireNotNull(daemon) { "Нет обновлённого демона патчей" }) }
        } else Ipsw(File(stage,"input.ipsw")).use {
            val path=Ipsw.path("RestoreRamDisk",it.buildIdentity()) ?: error("Нет исходного RestoreRamDisk")
            File(stage,"InfernoData/Restore/$path")
        }
        require(file.isFile && file.length()>0) { "Нет ramdisk для выбранного этапа" };return file
    }
}
