// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import dev.embervm.android.restore.PlistXml
import dev.embervm.android.restore.RestorePrep
import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.DigestInputStream
import java.security.MessageDigest
import java.util.UUID

/** Guest acceleration and Android framebuffer presentation are independent. */
enum class GuestGraphicsMode { SOFTWARE, SYSTEM_GLES }

/** A full independent kit, never an overlay dependent on a writable base.
 * COPIED is deliberately not ABI_VERIFIED, INSTALLED or accelerated. */
object SystemGpuProfile {
    const val BUILD="18A5351d"
    const val RESERVE_BYTES=256L*1024*1024
    private val idPattern=Regex("system-[a-f0-9]{32}")
    data class CopyResult(val config:VmConfig,val files:Int,val logicalBytes:Long)
    data class Progress(val file:String,val copied:Long,val total:Long)
    fun directory(privateDirectory:File)=File(privateDirectory,"system-gpu-profiles")

    fun create(source:VmConfig,privateDirectory:File,
        id:String="system-"+UUID.randomUUID().toString().replace("-",""),
        cancelled:()->Boolean={Thread.currentThread().isInterrupted},note:(Progress)->Unit={}):CopyResult {
        VmLease.acquire(privateDirectory).use {
            require(idPattern.matches(id)) { "Invalid GPU profile identifier" }
            val base=directory(privateDirectory).canonicalFile
            val original=source.root.canonicalFile.toPath()
            require(!base.toPath().startsWith(original) && !original.startsWith(base.toPath())) { "GPU profile must be outside the active kit" }
            require(!Files.isSymbolicLink(source.root.toPath())) { "Symlink kit root" }
            require(!File(source.root,"restore-prep.lock").exists() && !File(source.root,"restore-activation.plist").exists()) { "Подготовка набора ещё не завершена" }
            val manifest=File(source.dataDirectory,"Restore/BuildManifest.plist")
            require(manifest.isFile && manifest.length() in 1..4L*1024*1024) { "Для GPU-профиля нужен BuildManifest установленного набора" }
            require((Plist.parse(manifest) as? Map<*,*>)?.get("ProductBuildVersion")==BUILD) { "Системный GPU-профиль поддерживает только $BUILD" }
            require(source.systemInstalled) { "Нет записанного системного диска" }
            val image=source.rootImage ?: error("Нет root")
            checkRoot(image)
            val fw=source.firmware
            val paths=linkedSetOf("InfernoData/${image.file.name}")
            for((name,size) in RestorePrep.diskSizes)if(name!="root") {
                require(File(source.dataDirectory,name).length()==size) { "Неполный набор: $name" }
                paths+="InfernoData/$name"
            }
            paths+=listOf("AppleSEPROM-Cebu-B1","InfernoData/root_ticket.der",
                "InfernoData/sep-firmware.n104.RELEASE.new.img4","InfernoData/Restore/BuildManifest.plist",
                "InfernoData/${fw.kernel}","InfernoData/${fw.deviceTree}","InfernoData/${fw.trustcache}",
                "InfernoData/Restore/038-44135-124.dmg")
            val inputs=paths.associateWith { name ->
                val file=File(source.root,name)
                var part=source.root.toPath()
                for(component in name.split('/')){part=part.resolve(component);require(!Files.isSymbolicLink(part)) { "Symlink component: $name" }}
                require(file.isFile && file.length()>0 && file.canonicalFile.toPath().startsWith(original)) { "Нет компонента $name" }
                file
            }
            var total=0L;for(file in inputs.values)total=Math.addExact(total,file.length())
            require(base.isDirectory || base.mkdirs())
            require(base.usableSpace>RESERVE_BYTES) { "Недостаточно места для копии iOS" }
            val target=File(base,id);val stage=File(base,"$id.part")
            require(!target.exists() && !stage.exists()) { "GPU profile already exists" }
            check(stage.mkdir());var copied=0L
            try {
                val hashes=linkedMapOf<String,String>()
                for((name,input) in inputs) {
                    if(cancelled())throw ImportCancelled()
                    val length=input.length();val stamp=input.lastModified();val digest=MessageDigest.getInstance("SHA-256")
                    val output=File(stage,name)
                    input.inputStream().use { file -> DigestInputStream(file,digest).use { stream ->
                        SparseCopy.copy(stream,output,onBytes={ n ->
                            copied+=n
                            require(stage.usableSpace>RESERVE_BYTES) { "Недостаточно места для копии iOS" }
                            note(Progress(name,copied,total))
                        },cancelled=cancelled)
                    } }
                    require(input.length()==length && input.lastModified()==stamp && output.length()==length) { "Исходный набор изменился при копировании" }
                    val expected=digest.digest();val check=MessageDigest.getInstance("SHA-256")
                    DigestInputStream(output.inputStream(),check).use { read ->
                        val buf=ByteArray(1 shl 20)
                        while(true){if(cancelled())throw ImportCancelled();if(read.read(buf)<0)break}
                    }
                    require(expected.contentEquals(check.digest())) { "Ошибка проверки копии $name" }
                    hashes[name]=expected.joinToString("") { "%02x".format(it) }
                }
                val metadata=PlistXml.encode(mapOf("Schema" to 1,"State" to "COPIED","Build" to BUILD,
                    "ProfileID" to id,"GuestGraphicsMode" to GuestGraphicsMode.SOFTWARE.name,
                    "Source" to source.root.canonicalPath,"LogicalBytes" to copied,"FilesSHA256" to hashes))
                File(stage,"copied.plist").outputStream().use { it.write(metadata);it.fd.sync() }
                if(cancelled())throw ImportCancelled()
                Files.move(stage.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE)
                return CopyResult(VmConfig(target),inputs.size,copied)
            } finally { if(stage.exists())check(stage.deleteRecursively()) { "Не удалось убрать незавершённую копию" } }
        }
    }

    private fun checkRoot(root:VmConfig.RootImage) {
        require(root.file.length() in 65536..(128L shl 30)) { "Неожиданный размер root" }
        if(root.format=="qcow2")RandomAccessFile(root.file,"r").use {
            require(it.readInt()==0x514649fb && it.readInt() in 2..3)
            require(it.readLong()==0L && it.readInt()==0) { "QCOW2 с backing chain нельзя копировать как самостоятельный профиль" }
        }
    }
}
