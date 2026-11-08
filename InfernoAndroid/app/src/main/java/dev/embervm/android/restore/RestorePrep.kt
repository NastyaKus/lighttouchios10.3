// SPDX-License-Identifier: GPL-3.0-or-later
// Port of RestorePrep.swift @ 4bc3970; staged output protects the active kit.
package dev.embervm.android.restore

import java.io.File
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption

object RestorePrep {
    const val MIN_SPACE=15L*1024*1024*1024
    val diskSizes=linkedMapOf("root" to (32L shl 30),"firmware" to (8L shl 20),"syscfg" to (128L shl 10),"ctrl_bits" to 8192L,"nvram" to 8192L,"effaceable" to 4096L,"panic_log" to (1L shl 20),"sep_nvram" to (64L shl 10),"sep_ssc" to (128L shl 10))
    fun staging(root:File)=File(root,"restore-staging")
    data class Input(val ipsw:()->InputStream,val ticket:()->InputStream,val rom:()->InputStream,val key:SepFirmware.Key,val daemon:ByteArray?,val patcher:ByteArray?)
    fun prepare(root:File,input:Input,note:(String)->Unit={}) {
        require(root.usableSpace>=MIN_SPACE) { "Нужно не менее 15 ГиБ свободного места" }
        val stage=staging(root);require(!File(stage,"restore-running").exists()) { "Восстановление уже выполняется" }
        val lock=File(root,"restore-prep.lock");require(lock.createNewFile()) { "Подготовка уже выполняется; после аварии удалите restore-prep.lock" }
        try {
            stage.deleteRecursively();stage.mkdirs();val data=File(stage,"InfernoData").apply { mkdirs() }
            val archive=File(stage,"input.ipsw");note("Копирование IPSW")
            copy(input.ipsw(),archive,12L shl 30) { require(root.usableSpace>8L shl 30) { "Недостаточно места для восстановления" } }
            val shsh=input.ticket().use { readBounded(it,8*1024*1024) }
            copy(input.rom(),File(stage,"AppleSEPROM-Cebu-B1"),16L shl 20)
            val rom=File(stage,"AppleSEPROM-Cebu-B1");require(rom.length()>=4096) { "SEP ROM слишком мал" }
            Ipsw(archive).use { ipsw ->
                val identity=ipsw.buildIdentity();val os=Ipsw.path("OS",identity) ?: error("В IPSW нет OS")
                val osRange=ipsw.rangeOfStored(os);require(osRange.length>0)
                val restore=File(data,"Restore").apply { mkdirs() }
                val paths=ipsw.members.keys.filterNot { it==os || it.contains("Cryptex",true) || it.endsWith(".dmg",true) && it!=Ipsw.path("RestoreRamDisk",identity) }
                for(path in paths) {
                    checkInterrupted();val member=ipsw.member(path)
                    require(member.plainSize<=512L*1024*1024) { "Неожиданный большой компонент $path" }
                    note("Извлечение $path");ipsw.extract(path,File(restore,path))
                }
                note("Создание тикетов эмулятора")
                File(data,"root_ticket.der").writeBytes(Ticket.forgeAP(shsh,identity))
                val sepPath=Ipsw.path("RestoreSEP",identity) ?: Ipsw.path("SEP",identity) ?: error("Нет SEP")
                val sepTicket=Ticket.forgeSEP(shsh,identity)
                File(data,"sep-firmware.n104.RELEASE.new.img4").writeBytes(SepFirmware.rebuild(ipsw.read(sepPath),input.key,sepTicket))
                val ramdisk=File(restore,Ipsw.path("RestoreRamDisk",identity) ?: error("Нет RestoreRamDisk"))
                val patched=input.daemon!=null && input.patcher!=null
                if(patched) {
                    note("Правка HFSX ramdisk")
                    RestoreRamdisk.build(ramdisk,File(stage,"restore-ramdisk.im4p"),input.daemon!!,input.patcher!!)
                } else ramdisk.copyTo(File(stage,"restore-ramdisk.im4p"),overwrite=true)
                for((name,size) in diskSizes) { checkInterrupted();RandomAccessFile(File(data,name),"rw").use { it.setLength(size) } }
                // No real-device SEP/NVRAM seeds: those break data-volume unlock.
                File(stage,"prepared.plist").writeBytes(PlistXml.encode(mapOf("Build" to "18A5351d","GuestTools" to patched,"State" to "PREPARED","PreparationID" to java.util.UUID.randomUUID().toString())))
                note(if(patched)"Набор подготовлен. iOS ещё не восстановлена." else "Набор подготовлен без патчеров ФС. Для запуска понадобятся патчи.")
            }
        } finally { input.key.clear();lock.delete() }
    }
    private fun readBounded(input:InputStream,max:Int):ByteArray {
        val out=java.io.ByteArrayOutputStream();val b=ByteArray(65536)
        while(true) { checkInterrupted();val n=input.read(b);if(n<0)break;require(out.size().toLong()+n<=max);out.write(b,0,n) };return out.toByteArray()
    }
    private fun copy(input:InputStream,target:File,max:Long,check:()->Unit={}) {
        val part=File(target.path+".part")
        try { input.use { src -> part.outputStream().use { out ->
            val b=ByteArray(1024*1024);var total=0L
            while(true) { checkInterrupted();check();val n=src.read(b);if(n<0)break;total+=n;require(total<=max) { "Входной файл слишком большой" };out.write(b,0,n) }
        } };Files.move(part.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING) } finally { part.delete() }
    }
    private fun checkInterrupted() { if(Thread.currentThread().isInterrupted)throw InterruptedException("Подготовка отменена") }
}

/** The daemon logs 'done' even on a non-zero exit: require all success evidence. */
class PatchEvidence {
    var exitCode:Int?=null;private set
    var planVerified=false;private set
    var cleanupComplete=false;private set
    val failureReady get()=cleanupComplete && exitCode!=null && exitCode!=0
    var services=false;private set
    var done=false;private set
    var panic:String?=null;private set
    var error:String?=null;private set
    var waitingForVolumes=false;private set
    var volumesReady=false;private set
    var firmwareGate=false;private set
    var storageInitialized=false;private set
    private var volumeWaitSince:Long?=null
    fun volumesTimedOut(now:Long):Boolean {
        if(!waitingForVolumes || volumesReady)return false
        val start=volumeWaitSince ?: now.also { volumeWaitSince=it }
        return now-start>=180_000
    }
    val successful get()=planVerified && (exitCode==null || exitCode==0) && cleanupComplete && services && done && panic==null && error==null
    fun accept(line:String) {
        if(line.trim()=="*** PATCHER: patch plan verified 18A5351d-v031-software all=45")planVerified=true
        if(line.trim()=="*** PATCHER: cleanup complete")cleanupComplete=true
        if(line.startsWith("*** PATCHER: ERROR:"))error=line.take(1024)
        if(line.trimStart().startsWith("panic(cpu"))panic=line.trim().take(1024)
        if(line.contains("FW update not complete, create dummy block device") && !storageInitialized)firmwareGate=true
        if(line.contains("Skipped firmware update successfully")) { storageInitialized=true;firmwareGate=false }
        if(line.contains("*** PATCHER: waiting for the restore to finish"))waitingForVolumes=true
        if(line.contains("*** PATCHER: the data volume is here") || line.contains("*** PATCHER: the system volume is here")) { volumesReady=true;waitingForVolumes=false }
        Regex("\\*\\*\\* PATCHER: the patcher exited with (-?\\d+)").find(line)?.let { exitCode=it.groupValues[1].toInt() }
        if(line.contains("*** PATCHER: five services switched off"))services=true
        if(line.contains("*** PATCHER: done, the machine can be reset now"))done=true
    }
}
