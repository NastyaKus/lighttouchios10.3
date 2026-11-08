package dev.embervm.android.restore

import dev.embervm.android.*
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.time.Instant
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class FormatsTest {
    private fun fixture(name:String)=javaClass.getResourceAsStream("/restore/$name")!!.readBytes()
    private fun file(name:String)=Files.createTempFile("ember-",name).toFile().apply { writeBytes(fixture(name));deleteOnExit() }
    private fun hex(s:String)=s.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private inline fun rejects(block:()->Unit) { try { block();fail("Expected rejection") } catch(_:IllegalArgumentException) { } catch(_:IllegalStateException) { } }
    @Suppress("UNCHECKED_CAST") private fun identity()=Plist.parse(fixture("identity.plist").inputStream()) as Map<String,Any?>

    @Test fun derLengthsUnsignedIntegerAndPrivateTag() {
        for(n in listOf(0,1,127,128,255,256,65536)) {
            val b=Der.encode(Der.octets(ByteArray(n){it.toByte()}));assertArrayEquals(ByteArray(n){it.toByte()},Der.parse(b).value)
        }
        assertArrayEquals(hex("02020080"),Der.encode(Der.integer(128)))
        val node=Der.tagged("MANB",listOf(Der.ia5("MANB"),Der.set(emptyList())))
        assertEquals("MANB",Der.parse(Der.encode(node)).fourCC)
        assertArrayEquals(hex("ff84ea859c420a300816044d414e423100"),Der.encode(node))
    }
    @Test fun derRejectsTruncationIndefiniteTrailingAndDeepNesting() {
        for(b in listOf(hex("0480"),hex("048201"),hex("0402ff"),hex("040000"),hex("ff8080808080808080808000")))rejects { Der.parse(b) }
        var node=Der.octets(byteArrayOf());repeat(130) { node=Der.sequence(listOf(node)) };rejects { Der.parse(Der.encode(node)) }
    }
    @Test fun img4RetainsPayloadAndTicket() {
        val raw=fixture("payload.im4p");val t=Der.encode(Der.sequence(listOf(Der.ia5("IM4M"),Der.integer(0))))
        val result=Der.parse(Img4.make(raw,t)).children!!
        assertEquals("IMG4",result[0].text);assertArrayEquals(raw,Der.encode(result[1]));assertArrayEquals(t,Der.encode(result[2].children!!.single()))
        assertArrayEquals(byteArrayOf(112,97,121,108,111,97,100,0,-1),Img4.readIM4P(raw).data)
        rejects { Img4.make(raw,raw) }
    }
    @Test fun aes256CbcNistSp80038A() {
        val key=SepFirmware.Key(hex("000102030405060708090a0b0c0d0e0f"),hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"))
        val ciphertext=hex("f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b")
        val plain=hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710")
        assertArrayEquals(plain,SepFirmware.decrypt(ciphertext,key));rejects { SepFirmware.decrypt(byteArrayOf(1),key) };key.clear();assertTrue(key.key.all { it==0.toByte() })
        rejects { SepFirmware.Key.parse("a".repeat(95)) };rejects { SepFirmware.Key.parse("z".repeat(96)) }
    }
    @Test fun xmlRoundTripDataEscapingTypesAndBadInput() {
        val p=mapOf("<&\"" to "a<&>\"'","data" to byteArrayOf(0,1,-1),"list" to listOf(true,false,12L,2.5),"date" to Instant.parse("2020-08-19T00:00:00Z"))
        val read=PlistWire.decode(PlistXml.encode(p));assertEquals("a<&>\"'",read["<&\""]);assertArrayEquals(byteArrayOf(0,1,-1),read["data"] as ByteArray)
        assertEquals(listOf(true,false,12L,2.5),read["list"]);assertEquals("2020-08-19T00:00:00Z",read["date"])
        for(text in listOf("<plist><dict><key>a</key></dict></plist>","<plist><array><string>a</string>","<plist><data>!!!</data></plist>","<plist><true/><false/></plist>","<plist><dict><key>a</key><true/><key>a</key><false/></dict></plist>"))rejects { Plist.parse(text.byteInputStream()) }
    }
    @Test fun storedDeflateZip64AndRangeBounds() {
        Ipsw(file("small.zip")).use {
            assertArrayEquals("0123456789".toByteArray(),it.read("stored"));assertEquals("hello".repeat(2000),String(it.read("deflated")))
            assertArrayEquals("345".toByteArray(),it.rangeOfStored("stored").read(3,3));rejects { it.rangeOfStored("stored").read(8,3) };rejects { it.rangeOfStored("deflated") }
            assertEquals("n104ap",(it.buildIdentity()["Info"] as Map<*,*>)["DeviceClass"])
        }
        Ipsw(file("zip64.zip")).use { assertEquals("zip64-data",String(it.read("os.dmg")));assertEquals(10L,it.rangeOfStored("os.dmg").length) }
    }
    @Test fun zipRejectsTraversalCrcAndTruncatedDirectory() {
        val bad=Files.createTempFile("badzip","zip").toFile()
        ZipOutputStream(bad.outputStream()).use { it.putNextEntry(ZipEntry("../escape"));it.write(byteArrayOf(1));it.closeEntry() };rejects { Ipsw(bad) }
        val valid=file("small.zip");val m=Ipsw(valid).member("stored")
        RandomAccessFile(valid,"rw").use { it.seek(m.dataOffset);it.write(255) };rejects { Ipsw(valid).read("stored") }
        bad.writeBytes(fixture("zip64.zip").dropLast(5).toByteArray());rejects { Ipsw(bad) };bad.delete()
    }
    @Test fun forgedTicketChangesDigestsAndSepIdentityButNotCertificateIdentity() {
        val ap=Ticket.forgeAP(fixture("ticket.shsh2"),identity());val sep=Ticket.forgeSEP(fixture("ticket.shsh2"),identity())
        fun all(n:Der.Node):List<Der.Node> = listOf(n)+(n.children?.flatMap { all(it) } ?: emptyList())
        val apNodes=all(Der.parse(ap));assertTrue(apNodes.any { it.value.contentEquals(ByteArray(20){1}) });assertTrue(apNodes.any { it.text=="atfr" })
        assertTrue(apNodes.any { it.value.contentEquals(byteArrayOf(42)) }) // AP identity untouched
        val top=Der.parse(sep);assertTrue(all(top).any { it.value.contentEquals(Der.integer(Ticket.ECID).value) })
        val extension=top.children!![4].children!![0].children!![0].children!![0].children!![0].children!![4].children!!.last()
        val nodes=all(Der.parse(extension.value));assertTrue(nodes.any { it.value.contentEquals(byteArrayOf(42)) });assertFalse(nodes.any { it.value.contentEquals(Der.integer(Ticket.ECID).value) })
        val output=File("build/parity").apply { mkdirs() };File(output,"ap.der").writeBytes(ap);File(output,"sep.der").writeBytes(sep);File(output,"personalized.img4").writeBytes(Img4.make(fixture("payload.im4p"),ap))
    }
    @Test fun ramdiskReplacesThreeRecordsAllocatesCompressedForkAndKeepsSize() {
        val stock=file("ramdisk.im4p");val result=File(stock.parentFile,stock.name+"patched")
        RestoreRamdisk.build(stock,result,"daemon-test".toByteArray(),"patcher-test".toByteArray())
        assertEquals(stock.length(),result.length());assertArrayEquals(fixture("ramdisk.im4p"),stock.readBytes())
        val payload=Img4.readIM4P(result.readBytes()).data
        assertEquals("daemon-test",String(payload,36*512,11));assertEquals("patcher-test",String(payload,40*512,12))
        assertEquals(210,ByteBuffer.wrap(payload,1072,4).order(ByteOrder.BIG_ENDIAN).int)
        assertArrayEquals(RestoreRamdisk.job,payload.copyOfRange(44*512,44*512+RestoreRamdisk.job.size))
        assertEquals(0xfc,payload[3*512+44/8].toInt() and 255)
        val output=File("build/parity").apply { mkdirs() };File(output,"ramdisk.im4p").writeBytes(result.readBytes());File(output,"job.plist").writeBytes(RestoreRamdisk.job)
        result.delete()
    }
    @Test fun ramdiskRejectsMalformedVolumeWithoutPublishing() {
        val stock=file("payload.im4p");val result=File(stock.path+".output")
        rejects { RestoreRamdisk.build(stock,result,byteArrayOf(1),byteArrayOf(2)) };assertFalse(result.exists());assertFalse(File(result.path+".part").exists())
    }
    @Test fun patcherDoneAloneAndNonzeroExitAreNotSuccess() {
        val p=PatchEvidence();p.accept("*** PATCHER: done, the machine can be reset now");assertFalse(p.successful)
        p.accept("*** PATCHER: the patcher exited with 7");p.accept("*** PATCHER: five services switched off");assertFalse(p.successful)
        val good=PatchEvidence();good.accept("*** PATCHER: the patcher exited with 0");good.accept("*** PATCHER: five services switched off");good.accept("*** PATCHER: patch plan verified 18A5351d-v031-software all=45");good.accept("*** PATCHER: cleanup complete");good.accept("*** PATCHER: done, the machine can be reset now");assertTrue(good.successful)
    }
    @Test fun activationBacksUpOldKitAndPromotesStagingOnly() {
        val root=Files.createTempDirectory("kit").toFile();val stage=RestorePrep.staging(root).apply { mkdirs() }
        File(root,"InfernoData").mkdirs();File(root,"InfernoData/root").writeText("old");File(root,"AppleSEPROM-Cebu-B1").writeText("old-rom")
        File(stage,"InfernoData").mkdirs();File(stage,"InfernoData/root").writeText("new");File(stage,"AppleSEPROM-Cebu-B1").writeText("new-rom")
        File(stage,"prepared.plist").writeText("prepared")
        InstallKit.activate(root,stage);assertEquals("new",File(root,"InfernoData/root").readText());assertFalse(File(stage,"prepared.plist").exists())
        val backup=root.listFiles()!!.single { it.name.startsWith("restore-backup-") };assertEquals("old",File(backup,"InfernoData/root").readText());root.deleteRecursively()
    }
    @Test fun restoreArgumentsDisableNetworkAndHoldSmcWithoutChangingNormalBoot() {
        val root=Files.createTempDirectory("argv").toFile();val cfg=VmConfig(root);val normal=cfg.arguments(VmSettings(),root);val restore=cfg.arguments(VmSettings(),root,File(root,"rdsk"))
        assertTrue(normal.any { it.contains("boot-mode=exit_recovery") });assertTrue(normal.contains("apple-ncm-host,netdev=net0,conn-addr=inferno-usb.sock"));assertFalse(normal.contains("-initrd"))
        assertTrue(restore.any { it.contains("boot-mode=enter_recovery") });assertTrue(restore.contains("driver=apple-smc,property=hold-reset,value=on"));assertTrue(restore.contains("-initrd"));assertFalse(restore.contains("-netdev"));root.deleteRecursively()
    }
    @Test fun interruptedActivationCanReturnPreviousKitWithoutDiscardingNewDisk() {
        val root=Files.createTempDirectory("recover").toFile();val backup=File(root,"restore-backup-123").apply { mkdirs() }
        File(root,"InfernoData").mkdirs();File(root,"InfernoData/root").writeText("new")
        File(backup,"InfernoData").mkdirs();File(backup,"InfernoData/root").writeText("old")
        File(root,"AppleSEPROM-Cebu-B1").writeText("old-rom")
        File(root,"restore-activation.plist").writeBytes(PlistXml.encode(mapOf("Backup" to backup.name,"Original" to listOf("InfernoData","AppleSEPROM-Cebu-B1"))))
        InstallKit.recover(root)
        assertEquals("old",File(root,"InfernoData/root").readText());assertEquals("old-rom",File(root,"AppleSEPROM-Cebu-B1").readText())
        assertEquals("new",File(backup,"unactivated/InfernoData/root").readText());assertFalse(File(root,"restore-activation.plist").exists());root.deleteRecursively()
    }
    @Test fun unsignedPlistIntegerSurvivesAndTruncatedHfsNeverPublishes() {
        val unsigned=java.math.BigInteger("18446744073709551615")
        assertEquals(unsigned,PlistWire.decode(PlistXml.encode(mapOf("Status" to unsigned)))["Status"])
        val stock=file("ramdisk.im4p");val bytes=stock.readBytes()
        val p=Img4.readIM4P(bytes);val volume=p.data.copyOf()
        // Extents-overflow catalog: refuse instead of wrapping offsets to its first extent.
        ByteBuffer.wrap(volume,1024+272+12,4).putInt(33)
        stock.writeBytes(Img4.makeIM4P(p.type,p.version,volume));val destination=File(stock.path+".output")
        rejects { RestoreRamdisk.build(stock,destination,byteArrayOf(1),byteArrayOf(2)) };assertFalse(destination.exists())
    }

    @Test fun zip64UsesLongOffsetsPastFourGiBWithoutExtractingASystemImage() {
        val bytes=fixture("zip64.zip")
        fun signature(value:String):Int {
            val needle=hex(value)
            return (0..bytes.size-needle.size).first { at -> needle.indices.all { bytes[at+it]==needle[it] } }
        }
        val central=signature("504b0102");val end64=signature("504b0606");val locator=signature("504b0607")
        val prefix=0x100000000L+4096
        val buffer=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val nameLength=buffer.getShort(central+28).toInt() and 65535
        buffer.putLong(central+46+nameLength+4+16,prefix) // ZIP64 local-header offset
        buffer.putLong(end64+48,prefix+central)
        buffer.putLong(locator+8,prefix+end64)
        val large=Files.createTempFile("zip64-sparse","zip").toFile()
        try {
            RandomAccessFile(large,"rw").use { it.seek(prefix);it.write(bytes) }
            Ipsw(large).use { zip ->
                val range=zip.rangeOfStored("os.dmg");assertTrue(range.offset>0xffffffffL)
                assertEquals("zip64-data",String(range.read(0,10)))
            }
        } finally { large.delete() }
    }

}
