// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import dev.embervm.android.restore.RestorePrep
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** A disposable kit. Never attaches a source root, firmware or NVRAM disk. */
object GpuLabKit {
    const val MIN_SPACE=512L*1024*1024
    private const val MARKER="EMBER-GPU-LAB-SCRATCH-v1"
    private const val STOCK="Restore/038-44135-124.dmg"
    val runName=Regex("run-[0-9]+-[a-f0-9]{12}")
    data class Kit(val config:VmConfig,val ramdisk:File,val rootFingerprint:String)

    fun sources(source:VmConfig):Map<String,File> {
        val fw=source.firmware
        return linkedMapOf(
            "AppleSEPROM-Cebu-B1" to source.sepRom,
            "InfernoData/root_ticket.der" to File(source.dataDirectory,"root_ticket.der"),
            "InfernoData/sep-firmware.n104.RELEASE.new.img4" to File(source.dataDirectory,"sep-firmware.n104.RELEASE.new.img4"),
            "InfernoData/${VmConfig.FALLBACK_FIRMWARE.kernel}" to File(source.dataDirectory,fw.kernel),
            "InfernoData/${VmConfig.FALLBACK_FIRMWARE.deviceTree}" to File(source.dataDirectory,fw.deviceTree),
            "InfernoData/${VmConfig.FALLBACK_FIRMWARE.trustcache}" to File(source.dataDirectory,fw.trustcache),
            "stock.im4p" to File(source.dataDirectory,STOCK),
        )
    }
    fun missing(source:VmConfig)=sources(source).filterValues { !it.isFile || it.length()==0L }.keys.toList()

    fun prepare(source:VmConfig,work:File,buildRamdisk:(File,File)->Unit,note:(String)->Unit={}):Kit {
        require(!work.exists()) { "Тестовая папка уже существует" }
        val src=source.root.canonicalFile.toPath();val dst=work.canonicalFile.toPath()
        require(!src.startsWith(dst) && !dst.startsWith(src)) { "Тест должен быть вне рабочего набора" }
        require(runName.matches(work.name)) { "Некорректное имя теста" }
        val manifest=File(source.dataDirectory,"Restore/BuildManifest.plist")
        if(manifest.isFile) {
            val version=(Plist.parse(manifest) as? Map<*,*>)?.get("ProductBuildVersion")
            require(version=="18A5351d") { "GPU-стенд поддерживает только iOS 14 18A5351d" }
        }
        val inputs=sources(source)
        require(missing(source).isEmpty()) { "Для GPU-теста не хватает: ${missing(source).joinToString()}. Нужен подготовленный набор 18A5351d." }
        require(inputs.values.all { it.length() in 1..(256L shl 20) }) { "Неожиданный размер компонента" }
        check(work.mkdirs());File(work,".ember-gpu-lab").writeText(MARKER)
        note("Копирование загрузочных компонентов")
        for((path,file) in inputs) {
            checkInterrupted()
            // Build the ramdisk directly from the read-only source, no extra 100 MB copy.
            if(path=="stock.im4p")continue
            val target=File(work,path).apply { parentFile!!.mkdirs() }
            file.inputStream().use { input -> target.outputStream().use { output -> input.copyTo(output,1024*1024) } }
        }
        val config=VmConfig(work);config.logDirectory.mkdirs()
        note("Создание временных дисков")
        for((name,size) in RestorePrep.diskSizes) {
            checkInterrupted();RandomAccessFile(File(config.dataDirectory,name),"rw").use { it.setLength(size) }
        }
        val root=File(config.dataDirectory,"root")
        RandomAccessFile(root,"rw").use {
            it.write(ByteArray(65536) { 0x5a }.also { b -> MARKER.toByteArray().copyInto(b) })
            it.seek(it.length()-65536);it.write(ByteArray(65536) { 0xa5.toByte() })
        }
        val ramdisk=File(work,"gpu-probe.im4p")
        note("Подготовка тестового ramdisk")
        checkInterrupted();buildRamdisk(inputs.getValue("stock.im4p"),ramdisk);checkInterrupted()
        require(config.missing().isEmpty() && ramdisk.isFile && ramdisk.length()>0)
        return Kit(config,ramdisk,fingerprint(root))
    }
    fun arguments(kit:Kit,qemuData:File,splitWx:Boolean):List<String> {
        require(kit.config.transferImage.isFile) { "GPU-хост ещё не создал канал" }
        return kit.config.arguments(VmSettings(cores=4,memoryGb=2,halfResolution=true,network=false,
            tbSizeMb=64,splitWx=splitWx,haptics=false,renderer=RendererBackend.CPU),qemuData,kit.ramdisk)
    }
    fun channelReady(file:File):Boolean=runCatching {
        require(file.length()==16L*1024*1024)
        val bytes=ByteArray(64);RandomAccessFile(file,"r").use { it.readFully(bytes) }
        val b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        bytes.copyOfRange(0,16).contentEquals("EMBER_GPU_PROBE1".toByteArray()) && b.getInt(16)==1 && b.getInt(20)==16*1024*1024
    }.getOrDefault(false)
    fun fingerprint(root:File):String {
        val hash=MessageDigest.getInstance("SHA-256")
        RandomAccessFile(root,"r").use {
            val b=ByteArray(65536);it.readFully(b);hash.update(b)
            it.seek(it.length()-b.size);it.readFully(b);hash.update(b)
        }
        return hash.digest().joinToString("") { "%02x".format(it) }
    }
    /** Delete only our marked direct child, including after interrupted runs. */
    fun cleanup(base:File,work:File) {
        require(!java.nio.file.Files.isSymbolicLink(work.toPath()))
        require(work.parentFile?.canonicalFile==base.canonicalFile && work.canonicalFile.parentFile==base.canonicalFile)
        require(runName.matches(work.name))
        if(!work.exists())return
        val marker=File(work,".ember-gpu-lab")
        require(marker.isFile && marker.readText()==MARKER) { "Нет маркера временного набора" }
        check(work.deleteRecursively()) { "Не удалось убрать временные файлы" }
    }
    private fun checkInterrupted(){if(Thread.currentThread().isInterrupted)throw InterruptedException("GPU-тест отменён")}
}
