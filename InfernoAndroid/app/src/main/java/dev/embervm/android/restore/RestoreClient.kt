// SPDX-License-Identifier: GPL-3.0-or-later
// Port of RestoreClient.swift @ 4bc3970, scoped to iOS 14 / 18A5351d.
package dev.embervm.android.restore

import dev.embervm.android.Plist
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.UUID
import java.util.zip.CRC32

object PlistWire {
    const val MAX=64*1024*1024
    fun send(channel:GuestChannel,body:Map<String,Any?>) {
        val bytes=PlistXml.encode(body);require(bytes.size in 1 until MAX)
        channel.write(GuestUsb.be32(bytes.size)+bytes)
    }
    @Suppress("UNCHECKED_CAST")
    fun decode(bytes:ByteArray):Map<String,Any?> = Plist.parse(bytes.inputStream()) as? Map<String,Any?> ?: error("Expected plist dictionary")
    fun receive(channel:GuestChannel,timeoutMs:Long=60_000):Map<String,Any?> {
        val size=GuestUsb.be32(channel.read(4,timeoutMs),0);require(size in 1 until MAX.toLong()) { "Invalid plist frame length $size" }
        return decode(channel.read(size.toInt(),timeoutMs))
    }
    class XmlStream(private val channel:GuestChannel) {
        private var buffer=byteArrayOf()
        fun receive(timeoutMs:Long=300_000):Map<String,Any?> {
            val until=System.nanoTime()/1_000_000+timeoutMs
            while(true) {
                val end=endOfPlist(buffer)
                if(end!=null) { var start=0;while(start<end && buffer[start].toInt() in listOf(9,10,13,32))start++
                    val raw=buffer.copyOfRange(start,end);buffer=buffer.copyOfRange(end,buffer.size);return decode(raw) }
                require(buffer.size<1024*1024) { "ASR XML too large" }
                val left=until-System.nanoTime()/1_000_000;require(left>0) { "ASR XML timeout" }
                buffer+=channel.readAvailable(left)
            }
        }
    }
    internal fun endOfPlist(bytes:ByteArray):Int? {
        val marker="</plist>".toByteArray()
        for(i in 0..bytes.size-marker.size) {
            if(marker.indices.all { bytes[i+it]==marker[it] }) {
                var end=i+marker.size
                while(end<bytes.size && (bytes[end]==10.toByte() || bytes[end]==13.toByte()))end++
                return end
            }
        }
        return null
    }
}

class RestoreClient(
    private val usb:GuestConnector,private val ipsw:Ipsw,private val identity:Map<String,Any?>,
    private val ticket:ByteArray,private val note:(String)->Unit={},private val progress:(String,Double)->Unit={_,_->}
) {
    fun run(restored:GuestChannel,protocolVersion:Int) {
        PlistWire.send(restored,startRestore(protocolVersion,identity))
        while(true) {
            val message=PlistWire.receive(restored,900_000)
            when(val type=message["MsgType"] as? String ?: error("restored: missing MsgType")) {
                "DataRequestMsg","AsyncDataRequestMsg" -> answer(message,restored)
                "ProgressMsg" -> progress(operationName((message["Operation"] as? Number)?.toInt()),((message["Progress"] as? Number)?.toDouble() ?: 0.0)/100)
                "StatusMsg" -> {
                    val status=(message["Status"] as? Number)?.toLong() ?: error("Missing final Status")
                    require(status==0L) { "restored failed: status $status" }
                    PlistWire.send(restored,mapOf("MsgType" to "ReceivedFinalStatusMsg"));note("restored: Status 0 подтверждён");return
                }
                "CrashLog","RestoredCrash" -> error("Guest restored crashed")
                "CheckpointMsg" -> note("Checkpoint ${message["CHECKPOINT_NAME"]}: ${message["CHECKPOINT_RESULT"]}")
                "PreviousRestoreLogMsg" -> note("Получен журнал предыдущего восстановления")
                else -> note("restored: $type")
            }
        }
    }
    @Suppress("UNCHECKED_CAST")
    private val manifest get() = identity["Manifest"] as? Map<String,Any?> ?: error("Missing Manifest")
    private fun raw(name:String)=ipsw.read(Ipsw.path(name,identity) ?: error("Missing component $name"))
    private fun personalized(name:String)=Img4.make(raw(name),ticket)
    private fun port(request:Map<String,Any?>):Int? = (request["DataPort"] as? Number)?.toInt()?.takeIf { it!=0 }?.also { require(it in 1..65535) }
    private fun reply(body:Map<String,Any?>,request:Map<String,Any?>,restored:GuestChannel) {
        val p=port(request)
        if(p==null)PlistWire.send(restored,body) else usb.connect(p,60_000).use { PlistWire.send(it,body) }
    }
    @Suppress("UNCHECKED_CAST")
    internal fun answer(request:Map<String,Any?>,restored:GuestChannel) {
        val type=request["DataType"] as? String ?: error("Missing DataType")
        val args=request["Arguments"] as? Map<String,Any?> ?: emptyMap();note("restored просит $type")
        val body:Map<String,Any?> = when(type) {
            "SystemImageData" -> { sendFilesystem(port(request) ?: 12345);return }
            "RootTicket" -> mapOf("RootTicketData" to ticket)
            "KernelCache","DeviceTree" -> mapOf("${type}File" to personalized(type))
            "SystemImageRootHash" -> mapOf("SystemImageRootHashFile" to personalized("SystemVolume"))
            "SystemImageCanonicalMetadata" -> mapOf("SystemImageCanonicalMetadataFile" to personalized("Ap,SystemVolumeCanonicalMetadata"))
            "NORData" -> norData(args)
            "BuildIdentityDict" -> mapOf("BuildIdentityDict" to identity,"Variant" to (args["Variant"] ?: "Erase"))
            "ReceiptManifest" -> mapOf("ReceiptManifest" to manifest)
            "FDRTrustData","FirmwareUpdaterPreflight","DeviceRestoreInfoPreflight" -> emptyMap()
            "PersonalizedData" -> imageData(args,"ImageList",null,"ImageData")
            "FUDData" -> imageData(args,"FUDImageList","IsFUDFirmware","FUDImageData")
            "EANData" -> imageData(args,"EANImageList","IsEarlyAccessFirmware","EANData")
            "PersonalizedBootObjectV3","SourceBootObjectV4","SourceBootObjectV5" -> { bootObject(args,request,restored,type=="PersonalizedBootObjectV3");return }
            else -> error("Unsupported restore data request: $type (iOS 14 only)")
        }
        reply(body,request,restored)
    }
    internal fun norData(args:Map<String,Any?>):Map<String,Any?> {
        val names=manifest.keys.filter { name ->
            val info=(manifest[name] as? Map<*,*>)?.get("Info") as? Map<*,*>
            info?.get("Path") is String && (info["IsFirmwarePayload"]==true || info["IsSecondaryFirmwarePayload"]==true && info["IsLoadedByiBoot"]==true)
        }.filterNot { it=="LLB" || it=="RestoreSEP" }.sorted().sortedBy { !it.startsWith("iBoot") }
        val images=names.associateWith { personalized(it) }
        val body=linkedMapOf<String,Any?>("LlbImageData" to personalized("LLB"),"NorImageData" to if(args.containsKey("FlashVersion1"))images else images.values.toList())
        for((c,k) in listOf("RestoreSEP" to "RestoreSEPImageData","SEP" to "SEPImageData","SepStage1" to "SEPPatchImageData")) {
            if(Ipsw.path(c,identity)!=null)body[k]=personalized(c)
        }
        return body
    }
    private fun imageData(args:Map<String,Any?>,list:String,kind:String?,data:String):Map<String,Any?> {
        val type=kind ?: args["ImageType"] as? String ?: error("Missing ImageType")
        val names=manifest.keys.sorted().filter { ((manifest[it] as? Map<*,*>)?.get("Info") as? Map<*,*>)?.get(type)==true }
        if(args[list]==true)return mapOf(list to names)
        val name=args["ImageName"] as? String
        if(name!=null)return if(name in names)mapOf("ImageName" to name,data to personalized(name)) else mapOf("ImageName" to name)
        return mapOf(data to names.associateWith { personalized(it) })
    }
    private fun bootObject(args:Map<String,Any?>,request:Map<String,Any?>,restored:GuestChannel,personalize:Boolean) {
        val name=args["ImageName"] as? String ?: error("Missing ImageName")
        val data=when(name) {
            "__RestoreVersion__" -> ipsw.read("RestoreVersion.plist")
            "__SystemVersion__" -> ipsw.read("SystemVersion.plist")
            "__GlobalManifest__" -> error("Global manifest requires Apple signing")
            else -> if(personalize)personalized(name) else raw(name)
        }
        val own=port(request)?.let { usb.connect(it,60_000) };val c=own ?: restored
        try { for(at in data.indices step 8192)PlistWire.send(c,mapOf("FileData" to data.copyOfRange(at,minOf(at+8192,data.size))));PlistWire.send(c,mapOf("FileDataDone" to true)) }
        finally { own?.close() }
    }
    private fun sendFilesystem(port:Int) {
        val range=ipsw.rangeOfStored(Ipsw.path("OS",identity) ?: error("Missing OS"))
        var opened:GuestChannel?=null;var failure:Exception?=null
        for(attempt in 1..30) {
            try { opened=usb.connect(port,20_000);break } catch(e:Exception) { failure=e;if(Thread.currentThread().isInterrupted)throw e;Thread.sleep(2000) }
        }
        val channel=opened ?: throw failure ?: error("ASR unavailable")
        channel.use { Asr.transfer(it,range,progress) }
    }
    companion object {
        private fun operationName(code:Int?)=when(code) {
            3 -> "Очистка";8 -> "Раздел с данными";12 -> "Прошивка NOR";13 -> "Раздел с системой"
            14 -> "Система";19 -> "Обновление SEP";21 -> "Проверка SEP";27 -> "Дерево устройства"
            else -> "Установка"
        }
        fun startRestore(protocolVersion:Int,identity:Map<String,Any?>,uuid:String=UUID.randomUUID().toString()):Map<String,Any?> {
            val info=identity["Info"] as? Map<*,*>
            val options=linkedMapOf<String,Any?>(
                "AutoBootDelay" to 0,"BootImageType" to "User","DFUFileType" to "RELEASE","DataImage" to false,
                "FirmwareDirectory" to ".","FlashNOR" to true,"KernelCacheType" to "Release","NORImageType" to "production",
                "RestoreBundlePath" to "/tmp/Per2.tmp","SystemImageType" to "User","UpdateBaseband" to true,"InstallDiags" to false,
                "PersonalizedDuringPreflight" to true,"RootToInstall" to false,"CreateFilesystemPartitions" to true,"SystemImage" to true,
                "UUID" to uuid,"SupportedDataTypes" to supportedDataTypes,"SupportedMessageTypes" to supportedMessageTypes,
                "SystemPartitionPadding" to (info?.get("SystemPartitionPadding") ?: mapOf("8" to 80,"16" to 160,"32" to 320,"64" to 640,"128" to 1280,"256" to 1280,"512" to 1280,"768" to 1280,"1024" to 1280))
            )
            return mapOf("Request" to "StartRestore","Label" to "Inferno","RestoreOptions" to options,"RestoreProtocolVersion" to protocolVersion)
        }
        // Boolean values are copied literally; false does not mean unsupported.
        val supportedDataTypes = linkedMapOf<String,Any?>(
            "AuthInstallCACert" to true,
            "BasebandBootData" to false,
            "BasebandData" to false,
            "BasebandStackData" to false,
            "BasebandUpdaterOutputData" to false,
            "BootabilityBundle" to false,
            "BootabilityBundleV2" to false,
            "BuildIdentityDict" to false,
            "BuildIdentityDictV2" to false,
            "Cryptex1LocalPolicy" to true,
            "DataType" to false,
            "DeviceRestoreInfoPreflight" to false,
            "DiagData" to false,
            "EANData" to false,
            "FDRMemoryCommit" to false,
            "FDRTrustData" to false,
            "FUDData" to false,
            "FileData" to false,
            "FileDataDone" to false,
            "FirmwareUpdaterData" to false,
            "FirmwareUpdaterDataV2" to false,
            "FirmwareUpdaterDataV3" to true,
            "FirmwareUpdaterPreflight" to true,
            "GrapeFWData" to false,
            "HPMFWData" to false,
            "HostSystemTime" to true,
            "KernelCache" to false,
            "MessageUseStreamedImageFile" to true,
            "NORData" to false,
            "NitrogenFWData" to true,
            "OpalFWData" to false,
            "OverlayRootDataCount" to false,
            "OverlayRootDataForKey" to true,
            "OverlayRootDataForKeyIndex" to true,
            "PeppyFWData" to true,
            "PersonalizedBootObjectV3" to false,
            "PersonalizedData" to true,
            "ProvisioningData" to false,
            "RamdiskFWData" to true,
            "ReceiptManifest" to true,
            "RecoveryOSASRImage" to true,
            "RecoveryOSAppleLogo" to true,
            "RecoveryOSDeviceTree" to true,
            "RecoveryOSFileAssetImage" to true,
            "RecoveryOSIBEC" to true,
            "RecoveryOSIBootFWFilesImages" to true,
            "RecoveryOSImage" to true,
            "RecoveryOSKernelCache" to true,
            "RecoveryOSLocalPolicy" to true,
            "RecoveryOSOverlayRootDataCount" to false,
            "RecoveryOSRootTicketData" to true,
            "RecoveryOSStaticTrustCache" to true,
            "RecoveryOSVersionData" to true,
            "RestoreLocalPolicy" to true,
            "RootData" to false,
            "RootTicket" to false,
            "S3EOverride" to false,
            "SourceBootObjectV3" to false,
            "SourceBootObjectV4" to false,
            "SourceBootObjectV5" to false,
            "SsoServiceTicket" to false,
            "StockholmPostflight" to false,
            "SystemImageCanonicalMetadata" to false,
            "SystemImageData" to false,
            "SystemImageRootHash" to false,
            "URLAsset" to true,
            "USBCFWData" to false,
            "USBCOverride" to false,
            "UpdateVolumeOverlayRootDataCount" to true,
        )
        val supportedMessageTypes = linkedMapOf<String,Any?>(
            "AsyncDataRequestMsg" to true,
            "AsyncWait" to true,
            "BBUpdateStatusMsg" to false,
            "CheckpointMsg" to true,
            "CrashLog" to true,
            "DataRequestMsg" to false,
            "FDRSubmit" to true,
            "MsgType" to false,
            "PreviousRestoreLogMsg" to false,
            "ProgressMsg" to false,
            "ProvisioningAck" to false,
            "ProvisioningInfo" to false,
            "ProvisioningStatusMsg" to false,
            "ReceivedFinalStatusMsg" to false,
            "RestoreAttestation" to true,
            "RestoreProtocol" to true,
            "RestoredCrash" to true,
            "StatusMsg" to false,
        )
    }
}

/** ASR has unframed XML, then raw bytes, optionally followed by SHA-1 per chunk. */
object Asr {
    const val CHUNK=131072
    fun packetInfo(size:Long,checksums:Boolean):Map<String,Any?> = linkedMapOf<String,Any?>(
        "FEC Slice Stride" to 40,"Packet Payload Size" to 1450,"Packets Per FEC" to 25,
        "Payload" to mapOf("Port" to 1,"Size" to size),"Stream ID" to 1,"Version" to 1
    ).apply { if(checksums)put("Checksum Chunk Size",CHUNK) }
    fun transfer(channel:GuestChannel,range:Ipsw.StoredRange,progress:(String,Double)->Unit={_,_->}) {
        val xml=PlistWire.XmlStream(channel);var checksums=false
        var first=true
        RandomAccessFile(range.file,"r").use { file ->
            loop@ while(true) {
                val packet=xml.receive();val cmd=packet["Command"] as? String
                require(!first || cmd=="Initiate") { "ASR must start with Initiate" };first=false
                when(cmd) {
                    "Initiate" -> { checksums=packet["Checksum Chunks"] as? Boolean ?: checksums;channel.write(PlistXml.encode(packetInfo(range.length,checksums))) }
                    "OOBData" -> {
                        var offset=(packet["OOB Offset"] as? Number)?.toLong() ?: error("ASR OOB Offset")
                        var count=(packet["OOB Length"] as? Number)?.toLong() ?: error("ASR OOB Length")
                        require(offset>=0 && count>=0 && offset<=range.length-count) { "ASR OOB outside OS image" }
                        file.seek(range.offset+offset)
                        while(count>0) { val n=minOf(count,CHUNK.toLong()).toInt();val b=ByteArray(n);file.readFully(b);channel.write(b);count-=n;offset+=n }
                    }
                    "Payload" -> break@loop
                    else -> error("Unsupported ASR command $cmd")
                }
            }
            file.seek(range.offset);var sent=0L;val crc=CRC32();var updated=0L;val started=System.nanoTime()
            while(sent<range.length) {
                val n=minOf(CHUNK.toLong(),range.length-sent).toInt();val data=ByteArray(n);file.readFully(data);crc.update(data)
                channel.write(if(checksums)data+MessageDigest.getInstance("SHA-1").digest(data) else data);sent+=n
                val now=System.nanoTime()/1_000_000
                if(now-updated>250 || sent==range.length) { val speed=sent.toDouble()/(1024*1024)/((System.nanoTime()-started)/1e9).coerceAtLeast(0.001)
                    progress("Образ системы · ${sent/(1024*1024)}/${range.length/(1024*1024)} МиБ · ${"%.1f".format(java.util.Locale.ROOT,speed)} МиБ/с",sent.toDouble()/range.length);updated=now }
            }
            require(crc.value==range.crc32) { "IPSW OS CRC mismatch; restoration cannot be accepted" }
        }
    }
}
