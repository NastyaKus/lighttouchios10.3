// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android.restore

import dev.embervm.android.*
import android.content.Context
import java.io.Closeable
import java.io.File
import java.io.RandomAccessFile
import java.util.concurrent.atomic.AtomicBoolean

/** Runs on one worker in :vm. No QEMU reinitialization in this process. */
class RestoreSession(private val context:Context,private val update:(String,Double)->Unit):Closeable {
    private val root=Prefs.root(context)
    private val stage=RestorePrep.staging(root)
    private val config=VmConfig(stage)
    private val cancelled=AtomicBoolean(false)
    @Volatile private var listener:NativeUnix.Listener?=null
    @Volatile private var usb:GuestUsb?=null
    @Volatile var engineExited=false
    @Volatile var engineExitStatus:Int?=null
    enum class Result { COMPLETE, NEEDS_PATCHES }
    fun run(patchOnly:Boolean=false):Result {
        val marker=File(stage,"restore-running")
        require(!File(root,"restore-prep.lock").exists()) { "Подготовка ещё выполняется" }
        val lease=RestoreAttempt.acquire(context.filesDir) // private storage, independent of external-storage FUSE locks
        try {
            RestoreAttempt.validatePrepared(root)
            if(patchOnly)RestoreCheckpoint.enterPatchBoot(root)
            val attemptID="${System.currentTimeMillis()}-${android.os.Process.myPid()}"
            RestoreAttempt.archivePrevious(root,attemptID)
            marker.writeText(attemptID) // informational only; a stale file is safe to replace
            update("Проверка подготовленного набора",0.0)
            require(Engine.state==Engine.STATE_IDLE) { "Виртуальная машина уже запущена" }
            require(BuildConfig.ENGINE_BUNDLED) { "В APK нет движка" }
            require(stage.usableSpace >= ((if(patchOnly)3L else 8L) shl 30)) { "Недостаточно места для выбранного этапа" }
            val prepared=Plist.parse(File(stage,"prepared.plist")) as? Map<*,*> ?: error("Invalid preparation marker")
            require(prepared["Build"]=="18A5351d")
            val hasTools=prepared["GuestTools"]==true
            val bootRamdisk=RestoreCheckpoint.ramdisk(root,patchOnly,if(patchOnly)context.assets.open("guest/inferno_patcher").use { it.readBytes() } else null)
            if(!patchOnly)RestoreCheckpoint.resetDisksForRestore(root)
            File(root,"restore-error.log").delete() // already archived; don't show the old failure as this run's result
            config.logDirectory.mkdirs();config.guestConsoleLog.writeText("")
            Engine.load()?.let { error("Движок: $it") }
            val sockets=File(context.cacheDir,"sock").apply { mkdirs() }
            listener=NativeUnix.Listener(File(sockets,VmConfig.USB_SOCKET)) // before QEMU dials, exactly once
            val args=config.arguments(Prefs.load(context),prepareQemuData(context),bootRamdisk)
            checkActive();update(if(patchOnly)"Загрузка ramdisk только для патчей; диски сохранены" else "Загрузка ramdisk восстановления",0.0)
            require(Engine.start(args,config.engineLog.path,sockets.path)) { "Engine.start failed" }
            if(!patchOnly) {
                var link:UsbLink?=null;val connectUntil=now()+600_000
                while(link==null) {
                    checkActive();require(now()<connectUntil) { "QEMU не подключился к USB за 10 минут" }
                    try { link=listener!!.accept() } catch(e:Exception) { checkActive();throw e }
                }
                usb=GuestUsb(link) { log(it) }
                val readyUntil=now()+900_000;update("Ожидание USB гостя",0.0)
                while(true) {
                    checkActive();require(now()<readyUntil) { "USB не появился за 15 минут" }
                    try { usb!!.enumerate();break }
                    catch(e:GuestUsb.Nak) { log("USB ещё не готов (NAK)");Thread.sleep(3000) }
                    catch(e:GuestUsb.Status) {
                        // Before the guest USB stack appears, the controller reports
                        // NODEV or STALL. Keep the one link; QEMU will not dial again.
                        if(e.code !in listOf(-1,-3))throw e
                        log("USB ещё не готов (${e.code})");Thread.sleep(3000)
                    }
                }
                update("Ожидание restored",0.0)
                var restored:GuestChannel?=null;val serviceUntil=now()+300_000
                while(restored==null) {
                    checkActive();require(now()<serviceUntil) { "restored не появился за 5 минут" }
                    try { restored=usb!!.connect(62078,20_000) } catch(e:java.io.IOException) { log(e.message ?: "restored unavailable");Thread.sleep(2000) }
                }
                Ipsw(File(stage,"input.ipsw")).use { ipsw ->
                    restored.use { channel ->
                        PlistWire.send(channel,mapOf("Request" to "QueryType","Label" to "Inferno"))
                        val response=PlistWire.receive(channel)
                        require(response["Type"]=="com.apple.mobile.restored") { "QueryType: unexpected ${response["Type"]}" }
                        val version=(response["RestoreProtocolVersion"] as? Number)?.toInt() ?: error("Missing restore protocol version")
                        require(version>0) { "Invalid restore protocol version" }
                        RestoreClient(usb!!,ipsw,ipsw.buildIdentity(),File(config.dataDirectory,"root_ticket.der").readBytes(),::log) { name,fraction ->
                            checkActive();update(name,fraction.coerceIn(0.0,1.0))
                        }.run(channel,version)
                    }
                }
                // The restore monitor calls /sbin/reboot after Status 0. Holding SMC
                // reset doesn't keep userspace alive: Darwin halts then panics. Stop
                // this boot, save proof, and run the patcher in a fresh :vm process.
                usb?.close();usb=null
                shutdown()
                require(engineExited && engineExitStatus==0) { "Не удалось остановить VM после restored Status 0; набор сохранён отдельно" }
                require(!cancelled.get()) { "Остановка перед сохранением результата" }
                RestoreCheckpoint.record(root)
                update(if(hasTools)"iOS записана. Нажмите «Завершить патчи» для отдельного запуска ramdisk." else
                    "iOS записана; программы патчей отсутствуют. Набор не активирован.",1.0)
                return Result.NEEDS_PATCHES
            }
            // With restored_external disabled there may be NO USB client at all.
            // Do not wait on accept here: monitor the console from the first byte.
            // A separate job runs nvmefwupdater --skip to expose the saved volumes.
            require(hasTools) { "Нет программ патчера" }
            log("PATCH-ONLY: existing disks preserved; USB accept/enumeration/StartRestore disabled; storage --skip job enabled")
            update("Патчи файловой системы",0.0)
            val evidence=PatchEvidence();val until=now()+3_600_000
            RandomAccessFile(config.guestConsoleLog,"r").use { console ->
                var line=StringBuilder()
                while(!evidence.successful) {
                    checkActive();require(now()<until) { "Нет подтверждения патчей ФС за час" }
                    var read=0
                    while(console.filePointer<console.length() && read++<1024*1024) {
                        val ch=console.read()
                        if(ch==10) {
                            val text=line.toString();evidence.accept(text)
                            if(text.contains("Skipped firmware update successfully")) {
                                log("PATCH-ONLY: NVMe initialization completed (--skip)")
                                if(!evidence.volumesReady)update("Патчи: накопитель готов, ожидание разделов",0.0)
                            }
                            if(text.contains("*** PATCHER:")) {
                                log(text)
                                update(when {
                                    text.contains("waiting for the restore") -> "Патчи: ожидание восстановленных разделов"
                                    text.contains("launchd cache") -> "Патчи: отключение служб"
                                    text.contains("patch plan verified ") -> "Патчи: все изменения кэша проверены"
                                    text.contains("sync before unmount ") -> "Патчи: сохранение данных перед отключением"
                                    text.contains("the patcher exited with ") && evidence.exitCode!=0 -> "Патчер завершился с ошибкой; ожидание отключения раздела"
                                    text.contains("unmounting ") -> "Патчи: отключение раздела, ожидание записи на диск"
                                    text.contains("mounting ") -> "Патчи: подключение раздела"
                                    text.contains("writing service overrides") -> "Патчи: отключение служб"
                                    text.contains("patching ") -> "Патчи: обработка системного кэша"
                                    text.contains("the data volume is here") -> "Патчи: разделы найдены, монтирование системы"
                                    else -> "Патчи файловой системы"
                                },0.0)
                            }
                            line=StringBuilder()
                        } else if(line.length<8192)line.append(ch.toChar())
                    }
                    require(evidence.error==null) { "Ошибка патчера: ${evidence.error}. Диски сохранены; сохраните диагностику." }
                    require(evidence.panic==null) { "Паника гостя во время патчей: ${evidence.panic}" }
                    require(!evidence.volumesTimedOut(now())) {
                        "Патчер не увидел разделы за 3 минуты${if(evidence.firmwareGate)" (FW update not complete)" else ""}. Диски сохранены; сохраните диагностику."
                    }
                    require(!evidence.failureReady) { "Патчер ФС завершился с кодом ${evidence.exitCode}" }
                    require(!evidence.done || evidence.successful) { "Патчер напечатал done без полного подтверждения успеха" }
                    Thread.sleep(200)
                }
            }
            update("Выключение машины перед активацией набора",1.0)
            shutdown()
            require(engineExited) { "PATCHED_AWAITING_SHUTDOWN: машина не остановилась; набор сохранён отдельно" }
            require(engineExitStatus==0) { "QEMU stopped with status $engineExitStatus; kit kept in staging" }
            InstallKit.activate(root,stage)
            update("Восстановление и патчи завершены. Первый запуск iOS ещё не проверен.",1.0)
            return Result.COMPLETE
        } finally {
            runCatching { usb?.close() };runCatching { listener?.close() }
            marker.delete();lease.close()
        }
    }
    private fun log(text:String) {
        val log=File(root,"restore.log")
        if(log.length()>4L*1024*1024)log.writeText(log.readText().takeLast(1024*1024))
        log.appendText(text.take(8192)+"\n")
    }
    private fun checkActive() { if(cancelled.get() || Thread.currentThread().isInterrupted)throw InterruptedException("Восстановление отменено");require(!engineExited) { "QEMU завершился раньше восстановления: $engineExitStatus" } }
    private fun shutdown() {
        Engine.requestShutdown();val deadline=now()+15_000
        while(!engineExited && now()<deadline && !cancelled.get())Thread.sleep(100)
    }
    override fun close() { cancelled.set(true);runCatching { usb?.close() };runCatching { listener?.close() } }
    private fun now()=System.nanoTime()/1_000_000
}

object InstallKit {
    /** Rename on one filesystem, with rollback. Old kit survives failed restore. */
    fun activate(root:File,stage:File) {
        val names=listOf("InfernoData","AppleSEPROM-Cebu-B1")
        require(names.all { File(stage,it).exists() })
        val backup=File(root,"restore-backup-${System.currentTimeMillis()}").apply { require(mkdirs()) }
        val movedOld=mutableListOf<String>();val movedNew=mutableListOf<String>()
        val journal=File(root,"restore-activation.plist")
        require(!journal.exists()) { "Нужно восстановить предыдущую активацию" }
        val original=names.filter { File(root,it).exists() }
        java.io.FileOutputStream(journal).use { out ->
            out.write(PlistXml.encode(mapOf("Backup" to backup.name,"Original" to original)));out.fd.sync()
        }
        try {
            for(name in names) {
                val active=File(root,name)
                if(active.exists()) { require(active.renameTo(File(backup,name))) { "Cannot back up $name" };movedOld+=name }
                require(File(stage,name).renameTo(active)) { "Cannot activate $name" };movedNew+=name
            }
            journal.delete()
            File(stage,"input.ipsw").delete() // only after both new files have been activated
            File(stage,"prepared.plist").delete() // cannot restart a kit already moved into the active location
        } catch(e:Exception) {
            for(name in movedNew.reversed())check(File(root,name).renameTo(File(stage,name))) { "Activation rollback failed; see restore-activation.plist" }
            for(name in movedOld.reversed())check(File(backup,name).renameTo(File(root,name))) { "Backup rollback failed; see restore-activation.plist" }
            journal.delete();throw e
        }
    }
    fun recover(root:File) {
        val journal=File(root,"restore-activation.plist")
        val record=dev.embervm.android.Plist.parse(journal) as? Map<*,*> ?: error("Activation journal is unreadable")
        val name=record["Backup"] as? String ?: error("Missing backup")
        require(Regex("restore-backup-[0-9]+").matches(name))
        val backup=File(root,name);val original=record["Original"] as? List<*> ?: error("Missing original entries")
        val names=listOf("InfernoData","AppleSEPROM-Cebu-B1");require(original.all { it in names })
        val unactivated=File(backup,"unactivated").apply { mkdirs() }
        for(entry in names) {
            val old=File(backup,entry);val active=File(root,entry)
            if(old.exists() || entry !in original) {
                if(active.exists())check(active.renameTo(File(unactivated,entry))) { "Cannot preserve unactivated $entry" }
                if(old.exists())check(old.renameTo(active)) { "Cannot restore backup $entry" }
            } else check(active.exists()) { "Missing original $entry; keep journal for manual inspection" }
        }
        File(RestorePrep.staging(root),"prepared.plist").delete()
        check(journal.delete())
    }

}
