package dev.embervm.android.restore

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.net.SocketTimeoutException
import java.security.MessageDigest
import java.util.zip.CRC32

class ProtocolTest {
    private class MemoryChannel(var input:ByteArray,private val fragment:Int=Int.MAX_VALUE):GuestChannel {
        val output=ByteArrayOutputStream();var closed=false
        override fun write(bytes:ByteArray) { output.write(bytes) }
        override fun read(count:Int,timeoutMs:Long):ByteArray { require(input.size>=count);return input.copyOfRange(0,count).also { input=input.copyOfRange(count,input.size) } }
        override fun readAvailable(timeoutMs:Long)=read(minOf(fragment,input.size),timeoutMs).also { require(it.isNotEmpty()) }
        override fun close() { closed=true }
    }
    @Test fun restoredLengthFrameAndAsrUnframedFragmentedCoalescedPlists() {
        val c=MemoryChannel(byteArrayOf());PlistWire.send(c,mapOf("Request" to "QueryType"));c.input=c.output.toByteArray()
        assertEquals("QueryType",PlistWire.receive(c)["Request"])
        val a=PlistXml.encode(mapOf("Command" to "Initiate"));val b=PlistXml.encode(mapOf("Command" to "Payload"))
        for(fragment in listOf(1,7,Int.MAX_VALUE)) {
            val stream=PlistWire.XmlStream(MemoryChannel(a+b,fragment));assertEquals("Initiate",stream.receive()["Command"]);assertEquals("Payload",stream.receive()["Command"])
        }
        try { PlistWire.receive(MemoryChannel(byteArrayOf(127,127,127,127)));fail() } catch(_:IllegalArgumentException) { }
    }
    @Test fun asrOobStreamingSha1ChunkBoundariesAndCrc() {
        val data=ByteArray(Asr.CHUNK+17) { (it*31).toByte() };val file=Files.createTempFile("asr","img").toFile().apply { writeBytes(byteArrayOf(7,8,9)+data+byteArrayOf(0)) }
        val crc=CRC32().apply { update(data) }.value
        val input=PlistXml.encode(mapOf("Command" to "Initiate","Checksum Chunks" to true))+PlistXml.encode(mapOf("Command" to "OOBData","OOB Offset" to 9,"OOB Length" to 13))+PlistXml.encode(mapOf("Command" to "Payload"))
        val c=MemoryChannel(input,13);Asr.transfer(c,Ipsw.StoredRange(file,3,data.size.toLong(),crc))
        val output=c.output.toByteArray();val end=PlistWire.endOfPlist(output)!!
        val info=PlistWire.decode(output.copyOfRange(0,end));assertEquals(131072L,info["Checksum Chunk Size"])
        val first=data.copyOfRange(0,Asr.CHUNK);val second=data.copyOfRange(Asr.CHUNK,data.size)
        val expected=data.copyOfRange(9,22)+first+MessageDigest.getInstance("SHA-1").digest(first)+second+MessageDigest.getInstance("SHA-1").digest(second)
        assertArrayEquals(expected,output.copyOfRange(end,output.size));file.delete()
    }
    @Test fun asrRejectsOobOutsideStoredOs() {
        val file=Files.createTempFile("asr","img").toFile().apply { writeBytes(ByteArray(10)) }
        val c=MemoryChannel(PlistXml.encode(mapOf("Command" to "Initiate"))+PlistXml.encode(mapOf("Command" to "OOBData","OOB Offset" to -1,"OOB Length" to 1)))
        try { Asr.transfer(c,Ipsw.StoredRange(file,0,10,0));fail() } catch(_:IllegalArgumentException) { };file.delete()
    }
    @Test fun startRestorePreservesLiteralConfiguratorOptionsAndFinalStatusAck() {
        val options=RestoreClient.startRestore(15,emptyMap(),"fixed")["RestoreOptions"] as Map<*,*>
        assertEquals(false,(options["SupportedDataTypes"] as Map<*,*>)["RootTicket"])
        assertEquals(false,(options["SupportedDataTypes"] as Map<*,*>)["NORData"])
        assertEquals(true,(options["SupportedDataTypes"] as Map<*,*>)["PersonalizedData"])
        assertEquals(0,options["AutoBootDelay"]);assertEquals("fixed",options["UUID"])
        val file=Files.createTempFile("client","zip").toFile().apply { writeBytes(ProtocolTest::class.java.getResourceAsStream("/restore/small.zip")!!.readBytes()) }
        val status=MemoryChannel(byteArrayOf());PlistWire.send(status,mapOf("MsgType" to "StatusMsg","Status" to 0))
        val restored=MemoryChannel(status.output.toByteArray());val usb=object:GuestConnector { override fun connect(port:Int,timeoutMs:Long):GuestChannel=error("Unexpected channel") }
        RestoreClient(usb,Ipsw(file),emptyMap(),byteArrayOf()).run(restored,15)
        val output=MemoryChannel(restored.output.toByteArray());assertEquals("StartRestore",PlistWire.receive(output)["Request"]);assertEquals("ReceivedFinalStatusMsg",PlistWire.receive(output)["MsgType"]);file.delete()
    }
    @Test fun missingOrNonzeroFinalStatusCannotSucceed() {
        val file=Files.createTempFile("client","zip").toFile().apply { writeBytes(ProtocolTest::class.java.getResourceAsStream("/restore/small.zip")!!.readBytes()) }
        for(status in listOf(mapOf<String,Any?>("MsgType" to "StatusMsg"),mapOf("MsgType" to "StatusMsg","Status" to 51))) {
            val incoming=MemoryChannel(byteArrayOf());PlistWire.send(incoming,status)
            try { RestoreClient(object:GuestConnector { override fun connect(port:Int,timeoutMs:Long):GuestChannel=error("unexpected") },Ipsw(file),emptyMap(),byteArrayOf()).run(MemoryChannel(incoming.output.toByteArray()),15);fail() } catch(_:IllegalArgumentException) { } catch(_:IllegalStateException) { }
        };file.delete()
    }

    @Test fun norOrderFudEmptyPreflightsAndFileDataChunks() {
        val file=Files.createTempFile("components","zip").toFile()
        val names=listOf("LLB","zFirmware","iBoot","KernelCache","RestoreSEP","SEP")
        val identity=mapOf<String,Any?>("Manifest" to names.associateWith { name ->
            mapOf("Info" to mapOf("Path" to "$name.im4p","IsFirmwarePayload" to true,"IsFUDFirmware" to (name=="zFirmware")))
        })
        java.util.zip.ZipOutputStream(file.outputStream()).use { zip ->
            for(name in names) {
                zip.putNextEntry(java.util.zip.ZipEntry("$name.im4p"));zip.write(Img4.makeIM4P(name.take(4).padEnd(4,'_'),"test",ByteArray(if(name=="KernelCache")20000 else 4){1}));zip.closeEntry()
            }
        }
        val ticket=Der.encode(Der.sequence(listOf(Der.ia5("IM4M"))))
        val usb=object:GuestConnector { override fun connect(port:Int,timeoutMs:Long):GuestChannel=error("Unexpected port") }
        val client=RestoreClient(usb,Ipsw(file),identity,ticket)
        val nor=client.norData(emptyMap());val images=nor["NorImageData"] as List<*>
        val first=Der.parse(images.first() as ByteArray).children!![1]
        assertEquals("iBoo",Img4.readIM4P(Der.encode(first)).type)
        assertTrue(client.norData(mapOf("FlashVersion1" to false))["NorImageData"] is Map<*,*>)
        for(type in listOf("FDRTrustData","FirmwareUpdaterPreflight","DeviceRestoreInfoPreflight")) {
            val c=MemoryChannel(byteArrayOf());client.answer(mapOf("DataType" to type),c);assertTrue(PlistWire.receive(MemoryChannel(c.output.toByteArray())).isEmpty())
        }
        val list=MemoryChannel(byteArrayOf());client.answer(mapOf("DataType" to "FUDData","Arguments" to mapOf("FUDImageList" to true)),list)
        assertEquals(listOf("zFirmware"),PlistWire.receive(MemoryChannel(list.output.toByteArray()))["FUDImageList"])
        val boot=MemoryChannel(byteArrayOf());client.answer(mapOf("DataType" to "SourceBootObjectV4","Arguments" to mapOf("ImageName" to "KernelCache")),boot)
        val receiver=MemoryChannel(boot.output.toByteArray());val data=ByteArrayOutputStream();var chunks=0
        while(true) { val message=PlistWire.receive(receiver);if(message["FileDataDone"]==true)break;val b=message["FileData"] as ByteArray;assertTrue(b.size<=8192);data.write(b);chunks++ }
        assertEquals(3,chunks);assertArrayEquals(Ipsw(file).read("KernelCache.im4p"),data.toByteArray());file.delete()
    }

    /** Independent byte-level virtual USB device, no Android classes or Apple files. */
    private class FakeUsb:UsbLink {
        var wire=byteArrayOf();var muxPending=ArrayDeque<ByteArray>();var setup=byteArrayOf()
        var deviceSeq=0;var maxOutgoing=0;var dataReceived=ByteArrayOutputStream();var tcpReceived=0L;var hostPort=0;var remotePort=0
        var nextStatus:Int?=null;var failShortOut=false;var sawZlp=false;var async=false;var badID=false;var window=1 // 256 bytes; force ACK-driven send window
        // Deliberately unlike the host header: do not teach the fake to repeat the
        // implementation's former assumption that both directions use feedface.
        var reservedWord=0L;var sequenceStart=0;var sequenceStep=1;var peerAck:Int?=null
        var fragmentLimit=Int.MAX_VALUE;var coalesceInput=false;var earlyV2Control=false
        var lastHostRx=-1
        var lastHostSequence=0;var expectedHostSequence=1;var clockMs=0L;var idleStepMs=0L
        var queueCapacity=Int.MAX_VALUE;var maxQueued=0;var outNakCount=0
        var dropDataAtSequence:Int?=null;var suppressTcpDataAck=false
        var duplicateAcceptedCount=0
        val duplicateHostSequences=mutableListOf<Int>();val droppedHostSequences=mutableListOf<Int>()
        val config=byteArrayOf(9,2,32,0,1,1,0,-128,50,9,4,0,0,2,-1,-2,2,0,7,5,-127,2,0,2,0,7,5,2,2,0,2,0)
        override fun read(count:Int,timeoutMs:Long):ByteArray { require(wire.size>=count) { "No fake USB reply" };return wire.copyOfRange(0,count).also { wire=wire.copyOfRange(count,wire.size) } }
        private fun int16(v:Int)=ByteBuffer.allocate(2).putShort(v.toShort()).array()
        private fun int32(v:Long)=ByteBuffer.allocate(4).putInt(v.toInt()).array()
        fun queueMux(proto:Int,body:ByteArray) {
            check(muxPending.size<queueCapacity) { "Fake guest IN endpoint queue overflow" }
            muxPending.add(int32(proto.toLong())+int32((body.size+16).toLong())+int32(reservedWord)+int16(deviceSeq)+int16(peerAck ?: lastHostSequence)+body)
            maxQueued=maxOf(maxQueued,muxPending.size)
            deviceSeq=(deviceSeq+sequenceStep) and 65535
        }
        fun tcp(flags:Int,payload:ByteArray=byteArrayOf(),sequence:Long=if(flags and 2!=0)100 else 101,acknowledged:Long=tcpReceived) {
            val body=int16(remotePort)+int16(hostPort)+int32(sequence)+int32(acknowledged)+byteArrayOf(0x50,flags.toByte())+int16(window)+ByteArray(4)+payload
            queueMux(6,body)
        }
        private fun muxOut(body:ByteArray) {
            maxOutgoing=maxOf(maxOutgoing,body.size);val protocol=GuestUsb.be32(body,0).toInt()
            if(protocol==0) {
                muxPending.add(int32(0)+int32(20)+int32(2)+int32(0)+int32(0))
                if(earlyV2Control)queueMux(1,byteArrayOf(0))
                return
            }
            require(GuestUsb.be32(body,8)==0xfeedfaceL) // HOST transmit contract stays intact
            val muxSequence=GuestUsb.be16(body,12)
            if(protocol==2) {
                require(body[16]==7.toByte() && muxSequence==0)
                deviceSeq=sequenceStart;lastHostSequence=0;expectedHostSequence=1;return
            }
            if(body.size>36 && dropDataAtSequence==muxSequence) {
                dropDataAtSequence=null;droppedHostSequences.add(muxSequence)
                queueMux(1,byteArrayOf(0));return // Emulated loss despite USB transfer success.
            }
            if(muxSequence!=expectedHostSequence) {
                if(((expectedHostSequence-muxSequence) and 65535) in 1..32767)duplicateAcceptedCount++
                duplicateHostSequences.add(muxSequence);queueMux(1,byteArrayOf(0));return
            }
            lastHostSequence=muxSequence;expectedHostSequence=(muxSequence+1) and 65535
            if(protocol!=6)return
            lastHostRx=GuestUsb.be16(body,14)
            hostPort=GuestUsb.be16(body,16);remotePort=GuestUsb.be16(body,18)
            val flags=body[29].toInt() and 255
            if(flags and 2 !=0) { tcpReceived=1;tcp(18) }
            else if(body.size>36) {
                dataReceived.write(body.copyOfRange(36,body.size));tcpReceived+=body.size-36
                if(suppressTcpDataAck)queueMux(1,byteArrayOf(0)) else tcp(16)
            }
        }
        override fun write(bytes:ByteArray) {
            clockMs++
            if(bytes.contentEquals(byteArrayOf(3)))return
            require(bytes.size>=23 && bytes[0]==1.toByte())
            val b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);b.get();val addr=b.get();val pid=b.int;val ep=b.get();val id=b.long;b.int;b.get();b.get();val want=b.short.toInt() and 65535
            require(want<=16384);val body=bytes.copyOfRange(23,bytes.size)
            require(pid==GuestUsb.IN || want==body.size)
            nextStatus?.let { status ->
                nextStatus=null
                wire+=ByteBuffer.allocate(21).order(ByteOrder.LITTLE_ENDIAN).put(2).put(addr).putInt(pid).put(ep).putLong(id).putInt(status).putShort(0).array()
                return
            }
            var response=byteArrayOf();var status=0;var length=want
            if(pid==GuestUsb.SETUP)setup=body
            if(pid==GuestUsb.IN && ep==0.toByte() && want>0) {
                val value=GuestUsb.le16(setup,2)
                response=when(value ushr 8) {
                    1 -> ByteArray(18).apply { this[0]=18;this[1]=1;this[17]=1 }
                    2 -> config.copyOfRange(0,minOf(want,config.size))
                    else -> error("Unknown descriptor")
                };length=response.size
            }
            if(pid==GuestUsb.OUT && ep==2.toByte()) {
                if(outNakCount>0) { outNakCount--;status=-2;length=0 }
                else if(body.isEmpty())sawZlp=true else muxOut(body)
                if(failShortOut)length=maxOf(0,want-1)
            }
            if(pid==GuestUsb.IN && ep==1.toByte()) {
                if(muxPending.isEmpty()) { status=-2;length=0;clockMs+=idleStepMs } else {
                    val limit=minOf(want,fragmentLimit)
                    while(muxPending.isNotEmpty() && response.size<limit) {
                        val first=muxPending.removeFirst();val take=minOf(first.size,limit-response.size)
                        response+=first.copyOfRange(0,take)
                        if(take<first.size)muxPending.addFirst(first.copyOfRange(take,first.size))
                        if(!coalesceInput)break
                    }
                    length=response.size
                }
            }
            val header=ByteBuffer.allocate(21).order(ByteOrder.LITTLE_ENDIAN).put(2).put(addr).putInt(pid).put(ep).putLong(if(badID)id+1 else id).putInt(status).putShort(length.toShort()).array()
            if(async)wire+=ByteBuffer.allocate(21).order(ByteOrder.LITTLE_ENDIAN).put(2).put(addr).putInt(pid).put(ep).putLong(id).putInt(-6).putShort(0).array()
            wire+=header+response
        }
        override fun close() { }
    }
    @Test fun usbEnumerationMuxHandshakeTcpWindowAnd16kPacketLimit() {
        val fake=FakeUsb();val usb=GuestUsb(fake);usb.enumerate()
        val channel=usb.connect(12345,2000);val data=ByteArray(200000){it.toByte()};channel.write(data)
        assertArrayEquals(data,fake.dataReceived.toByteArray());assertTrue(fake.maxOutgoing<=GuestUsb.MTU);assertFalse(fake.sawZlp)
        channel.close();usb.close()
        val wide=FakeUsb().apply { window=4096 };val host=GuestUsb(wide);host.enumerate();host.connect(12345,2000).write(data)
        assertArrayEquals(data,wide.dataReceived.toByteArray());assertTrue(wide.maxOutgoing in 16000..16384);assertFalse(wide.sawZlp);host.close()
    }
    @Test fun usbShortOutAndMalformedDescriptorFailExplicitly() {
        val fake=FakeUsb();fake.failShortOut=true
        try { GuestUsb(fake).enumerate();fail() } catch(_:IllegalArgumentException) { }
        try { GuestUsb.findMux(byteArrayOf(0,4));fail() } catch(_:IllegalArgumentException) { }
    }
    @Test fun usbAsyncPromiseCompletesSameTransferAndBadIdentityIsRejected() {
        val fake=FakeUsb().apply { async=true };val host=GuestUsb(fake);host.enumerate();host.connect(12345,2000).write(ByteArray(2000));assertEquals(2000,fake.dataReceived.size());host.close()
        val corrupt=FakeUsb().apply { badID=true }
        try { GuestUsb(corrupt).enumerate();fail() } catch(_:IllegalArgumentException) { }
    }

    @Test fun deviceMuxReservedWordCanDifferFromHostAndTraceIsBounded() {
        for(word in listOf(0L,0xfacefeedL,0xfeedfaceL,0xffffffffL)) {
            val fake=FakeUsb().apply { reservedWord=word;sequenceStart=41;peerAck=0x3456 }
            val notes=mutableListOf<String>();val usb=GuestUsb(fake,note=notes::add);usb.enumerate()
            val channel=usb.connect(62078,2000)
            assertEquals(41,fake.lastHostRx) // offset 12 echoed; offset 14 deliberately differs
            channel.write(ByteArray(12000))
            val headers=notes.filter { it.startsWith("USB MUX RX") }
            assertEquals(16,headers.size)
            assertTrue(headers.any { it.contains("reserved=0x${word.toString(16).padStart(8,'0')} seq12=41 seq14=13398") })
            usb.close()
        }
    }

    @Test fun repeatedSkippingAndWrappingMuxCountersStillDeliverDistinctTcpData() {
        for(step in listOf(0,1,3)) {
            val fake=FakeUsb().apply { sequenceStart=65535;sequenceStep=step;coalesceInput=true }
            val usb=GuestUsb(fake);usb.enumerate();val channel=usb.connect(62078,2000)
            fake.tcp(16,byteArrayOf(1),101)
            fake.tcp(16,byteArrayOf(1),101) // TCP duplicate must still be suppressed
            fake.tcp(16,byteArrayOf(2),102)
            assertArrayEquals(byteArrayOf(1,2),channel.read(2,2000));usb.close()
        }
    }

    @Test fun muxReassemblesSplitHeadersCoalescedFramesAndFramesLargerThanUsbMtu() {
        for((fragment,size) in listOf(1 to 40,7 to 517,16384 to 16521)) {
            val fake=FakeUsb().apply { fragmentLimit=fragment;coalesceInput=true;window=4096 }
            val usb=GuestUsb(fake);usb.enumerate();val channel=usb.connect(62078,2000)
            val data=ByteArray(size) { (it*29).toByte() }
            fake.queueMux(1,byteArrayOf(7)) // ignored control frame, potentially coalesced with TCP
            fake.tcp(16,data,101)
            assertArrayEquals(data,channel.read(data.size,5000));usb.close()
        }
    }

    @Test fun versionReplyAndFirstV2ControlCanShareOneUsbTransfer() {
        val fake=FakeUsb().apply { earlyV2Control=true;coalesceInput=true }
        val usb=GuestUsb(fake);usb.enumerate();usb.connect(62078,2000).write(byteArrayOf(1,2,3))
        assertArrayEquals(byteArrayOf(1,2,3),fake.dataReceived.toByteArray());usb.close()
    }

    @Test fun invalidMuxLengthsFailBeforeAllocatingOrWaitingForPayload() {
        for(size in listOf(0,15,4*1024*1024+1,-1)) {
            val fake=FakeUsb();val usb=GuestUsb(fake);usb.enumerate();val channel=usb.connect(62078,2000)
            fake.muxPending.add(ByteBuffer.allocate(16).putInt(6).putInt(size).putLong(0).array())
            try { channel.read(1,2000);fail("Accepted length $size") }
            catch(e:IllegalArgumentException) { assertTrue(e.message!!.startsWith("Invalid mux frame length=")) }
            usb.close()
        }
    }

    @Test fun tcpSequenceGapIsStillRejectedWithPermissiveMuxCounters() {
        val fake=FakeUsb();val usb=GuestUsb(fake);usb.enumerate();val channel=usb.connect(62078,2000)
        fake.tcp(16,byteArrayOf(1),102) // Expected TCP byte 101, irrespective of mux counters.
        try { channel.read(1,2000);fail() }
        catch(e:IllegalArgumentException) { assertEquals("TCP sequence gap/overlap",e.message) }
        usb.close()
    }

    @Test fun usbNodevBeforeGuestBootCanBeRetriedOnTheSameLink() {
        val fake=FakeUsb().apply { nextStatus=-1 };val host=GuestUsb(fake)
        try { host.enumerate();fail() } catch(e:GuestUsb.Status) { assertEquals(-1,e.code) }
        host.enumerate();host.connect(12345,2000).close();host.close()
    }

    @Test fun incomingEndpointIsServicedBeforeWritableTcpWindowFills() {
        val fake=FakeUsb().apply { window=4096;queueCapacity=2;reservedWord=0xfacefaceL }
        val usb=GuestUsb(fake);usb.enumerate();val channel=usb.connect(12345,2000)
        val bytes=ByteArray(200000) { (it*17).toByte() };channel.write(bytes)
        assertArrayEquals(bytes,fake.dataReceived.toByteArray())
        assertTrue(fake.maxQueued<=2);assertEquals(0,fake.duplicateAcceptedCount);usb.close()
    }

    @Test fun lostMuxFrameIsReplayedWithoutFloodingGuestOrReplayingAcceptedPrefix() {
        val fake=FakeUsb().apply { window=512;queueCapacity=2;idleStepMs=250;dropDataAtSequence=10 }
        val notes=mutableListOf<String>();val usb=GuestUsb(fake,clock={fake.clockMs},note=notes::add)
        usb.enumerate();val channel=usb.connect(12345,2000)
        val bytes=ByteArray(400000) { (it*19).toByte() };channel.write(bytes)
        assertArrayEquals(bytes,fake.dataReceived.toByteArray())
        assertEquals(listOf(10),fake.droppedHostSequences)
        assertEquals(0,fake.duplicateAcceptedCount)
        assertTrue(notes.any { it.contains("неподтверждённых mux-пакетов; peerAck=9 first=10") })
        assertTrue(fake.maxQueued<=2);usb.close()
    }

    @Test fun busyGuestThatAlreadyAcceptedMuxDataDoesNotTriggerBlindReplay() {
        val fake=FakeUsb().apply { window=4096;suppressTcpDataAck=true;idleStepMs=250;queueCapacity=2 }
        val notes=mutableListOf<String>();val usb=GuestUsb(fake,clock={fake.clockMs},note=notes::add)
        usb.enumerate();val channel=usb.connect(12345,2000);channel.write(ByteArray(256))
        try { channel.read(1,12000);fail() }
        catch(e:SocketTimeoutException) { assertEquals("TCP read",e.message) }
        assertEquals(256,fake.dataReceived.size());assertEquals(0,fake.duplicateAcceptedCount)
        assertFalse(notes.any { it.startsWith("USB: повтор") });usb.close()
    }

    @Test fun outNakDrainsIncomingDataWithoutSendingAheadOfTheBlockedMuxNumber() {
        val fake=FakeUsb();val usb=GuestUsb(fake);usb.enumerate();val channel=usb.connect(12345,2000)
        fake.tcp(16,byteArrayOf(7),101);fake.outNakCount=3
        val data=ByteArray(500) { it.toByte() };channel.write(data)
        assertArrayEquals(data,fake.dataReceived.toByteArray())
        assertArrayEquals(byteArrayOf(7),channel.read(1,2000))
        assertTrue(fake.duplicateHostSequences.isEmpty());assertEquals(0,fake.outNakCount);usb.close()
    }

    @Test fun genuinelyUnresponsiveOutStillStopsAt60SecondsWithUsefulHeaderTrace() {
        val fake=FakeUsb().apply { idleStepMs=10000 }
        val notes=mutableListOf<String>();val usb=GuestUsb(fake,clock={fake.clockMs},note=notes::add)
        usb.enumerate();val channel=usb.connect(12345,2000)
        fake.outNakCount=Int.MAX_VALUE;val started=fake.clockMs
        try { channel.write(byteArrayOf(1));fail() }
        catch(e:SocketTimeoutException) { assertEquals("USB OUT NAK 60s",e.message) }
        assertTrue(fake.clockMs-started in 60000..71000)
        assertTrue(notes.any { it.startsWith("USB OUT waiting") && it.contains("seq=3 peerAck=1") })
        assertEquals(0,fake.dataReceived.size());usb.close()
    }

    @Test fun staleTcpAckIsIgnoredButAckOfUnsentBytesStillFails() {
        for(future in listOf(false,true)) {
            val fake=FakeUsb();val usb=GuestUsb(fake);usb.enumerate();val channel=usb.connect(12345,2000)
            channel.write(ByteArray(256));assertEquals(257L,fake.tcpReceived)
            fake.tcp(16,acknowledged=if(future)258 else 1)
            if(future) {
                try { channel.read(1,2000);fail() }
                catch(e:IllegalArgumentException) { assertEquals("TCP acknowledgement outside sent window",e.message) }
            } else {
                fake.tcp(16,byteArrayOf(9),101)
                assertArrayEquals(byteArrayOf(9),channel.read(1,2000))
            }
            usb.close()
        }
    }

    @Test fun cumulativeMuxAckAndLossRecoveryWorkAcrossHostCounterWrap() {
        val fake=FakeUsb().apply { queueCapacity=2;idleStepMs=250;dropDataAtSequence=65535 }
        val usb=GuestUsb(fake,clock={fake.clockMs});usb.enumerate();val channel=usb.connect(12345,2000)
        val bytes=ByteArray(256) { 7 };val count=65540
        repeat(count) { channel.write(bytes) }
        assertEquals(count*bytes.size,fake.dataReceived.size())
        assertTrue(fake.dataReceived.toByteArray().all { it==7.toByte() })
        assertEquals(listOf(65535),fake.droppedHostSequences);assertEquals(0,fake.duplicateAcceptedCount)
        assertEquals(7,fake.expectedHostSequence);usb.close()
    }

}
