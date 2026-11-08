// SPDX-License-Identifier: GPL-3.0-or-later
// Port of GuestUSB.swift @ 4bc3970. One worker owns mux/TCP state.
package dev.embervm.android.restore

import java.io.Closeable
import java.io.EOFException
import java.io.IOException
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Injectable byte link: production is a Unix socket; tests emulate the device. */
interface UsbLink : Closeable {
    fun read(count:Int,timeoutMs:Long):ByteArray
    fun write(bytes:ByteArray)
}
interface GuestChannel : Closeable {
    fun write(bytes:ByteArray)
    fun read(count:Int,timeoutMs:Long=60_000):ByteArray
    fun readAvailable(timeoutMs:Long=60_000):ByteArray
}
interface GuestConnector { fun connect(port:Int,timeoutMs:Long=30_000):GuestChannel }

class GuestUsb(
    private val link:UsbLink,
    private val clock:()->Long={System.nanoTime()/1_000_000},
    private val note:(String)->Unit={}
):GuestConnector,Closeable {
    private var transferID=1L
    private var bootTraceBudget=0
    private var muxTraceBudget=0
    private fun bootTrace(text:String) {
        if(version<2 && bootTraceBudget>0) { bootTraceBudget--;note(text) }
    }
    private var epIn=0;private var epOut=0;private var maxPacket=512
    private var version=0;private var tx=0;private var rx=65535
    private var incoming=byteArrayOf()
    private val channels=linkedMapOf<Int,Channel>()
    private var nextPort=0xf001
    private val recent=ArrayDeque<ByteArray>()
    private var peerMuxAck=65535
    private fun now()=clock()
    private var progress=now();private var resent=now()
    @Volatile private var closed=false
    class Nak:IOException("USB NAK")
    class Status(val code:Int):IOException("USB status $code")
    private fun checkOpen() { if(closed || Thread.currentThread().isInterrupted)throw EOFException("USB closed") }
    internal fun transfer(pid:Int,ep:Int,data:ByteArray=byteArrayOf(),length:Int=data.size,retries:Int=1):ByteArray {
        require(length in 0..MTU && (pid==IN || data.size==length))
        repeat(retries) {
            checkOpen();val id=transferID++
            val request=ByteBuffer.allocate(23).order(ByteOrder.LITTLE_ENDIAN)
                .put(1).put(0).putInt(pid).put(ep.toByte()).putLong(id).putInt(0).put(0).put(0).putShort(length.toShort()).array()
            bootTrace("USB TX id=$id pid=0x${pid.toString(16)} ep=$ep length=$length")
            link.write(if(pid==IN)request else request+data)
            val until=now()+60_000
            while(true) {
                val kind=link.read(1,(until-now()).coerceAtLeast(1))[0].toInt() and 255
                require(kind==2) { "USB response kind $kind" }
                val h=ByteBuffer.wrap(link.read(20,(until-now()).coerceAtLeast(1))).order(ByteOrder.LITTLE_ENDIAN)
                val address=h.get().toInt() and 255;val token=h.int;val endpoint=h.get().toInt() and 255
                val reply=h.long;val status=h.int;val n=h.short.toInt() and 65535
                bootTrace("USB RX id=$reply pid=0x${token.toString(16)} ep=$endpoint status=$status length=$n")
                require(n<=MTU && address==0 && token==pid && endpoint==ep && reply==id) { "USB response identity/length mismatch" }
                val body=if(n>0 && status != -6 && token==IN)link.read(n,(until-now()).coerceAtLeast(1)) else byteArrayOf()
                if(status == -6) { if(now()>=until)throw SocketTimeoutException("USB ASYNC timeout");continue }
                if(status == -2)break
                if(status!=0)throw Status(status)
                require(if(pid==IN)n<=length else n==length) { "USB short/oversized transfer" }
                return body
            }
            if(it+1<retries)Thread.sleep(10)
        }
        throw Nak()
    }
    private fun control(type:Int,request:Int,value:Int,index:Int,length:Int=0):ByteArray {
        transfer(SETUP,0,ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).put(type.toByte()).put(request.toByte()).putShort(value.toShort()).putShort(index.toShort()).putShort(length.toShort()).array(),retries=200)
        val incoming=type and 128 !=0
        val out=if(length>0)transfer(if(incoming)IN else OUT,0,length=length,retries=200) else byteArrayOf()
        transfer(if(incoming)OUT else IN,0,length=0,retries=200)
        return out
    }
    fun enumerate() {
        checkOpen();version=0;incoming=byteArrayOf();tx=0;rx=65535;peerMuxAck=65535;recent.clear()
        bootTraceBudget=96;muxTraceBudget=16;bootTrace("USB RESET on existing socket")
        link.write(byteArrayOf(3));Thread.sleep(300)
        val device=control(128,6,0x100,0,18);require(device.size==18)
        val configurations=device[17].toInt() and 255;require(configurations in 1..16)
        for(index in configurations-1 downTo 0) {
            val head=control(128,6,0x200 or index,0,9);require(head.size>=9)
            val n=le16(head,2);require(n in 9..MTU)
            val raw=control(128,6,0x200 or index,0,n)
            val found=findMux(raw) ?: continue
            epIn=found.first;epOut=found.second;maxPacket=found.third
            control(0,9,raw[5].toInt() and 255,0)
            sendMux(0,be32(2)+be32(0)+be32(0))
            val deadline=now()+10_000
            while(now()<deadline) {
                val p=readMux()
                if(p==null) { Thread.sleep(1);continue }
                if(p.first==0 && p.second.size>=12) {
                    version=minOf(be32(p.second,0).toInt(),2);require(version==2) { "usbmux v2 required" }
                    sendMux(2,byteArrayOf(7));note("USB: usbmux v2 готов");return
                }
            }
            throw SocketTimeoutException("usbmux VERSION")
        }
        error("No usbmux interface")
    }
    private fun bulkOut(packet:ByteArray) {
        require(packet.size<=MTU);val started=now();val deadline=started+60_000
        var retries=0;var drained=0;var reportAt=started+2000
        while(true) {
            try {
                transfer(OUT,epOut,packet)
                if(retries>0 && now()-started>=2000)note("USB OUT resumed after ${now()-started} ms; retries=$retries drained=$drained")
                return
            } catch(_:Nak) {
                retries++
                if(now()>=reportAt) {
                    val sequence=if(version>=2 && packet.size>=16)be16(packet,12).toString() else "-"
                    note("USB OUT waiting ${now()-started} ms; bytes=${packet.size} seq=$sequence peerAck=$peerMuxAck pending=${recent.size} retries=$retries drained=$drained buffered=${incoming.size}")
                    reportAt=now()+10_000
                }
                if(now()>=deadline)throw SocketTimeoutException("USB OUT NAK 60s")
                drained+=drainIn();Thread.sleep(1)
            }
        }
    }
    private fun append(bytes:ByteArray) { require(incoming.size+bytes.size<=MAX_MUX_FRAME) { "Mux input overflow" };incoming+=bytes }
    private fun drainIn():Int {
        return try { val bytes=transfer(IN,epIn,length=MTU);append(bytes);bytes.size } catch(_:Nak) { 0 }
    }
    private fun sendMux(proto:Int,body:ByteArray) {
        if(proto==2) { tx=0;rx=65535 }
        val head=ByteBuffer.allocate(if(version<2)8 else 16).order(ByteOrder.BIG_ENDIAN).putInt(proto).putInt((if(version<2)8 else 16)+body.size)
        if(version>=2)head.putInt(0xfeedface.toInt()).putShort(tx.toShort()).putShort(rx.toShort())
        val packet=head.array()+body;bulkOut(packet)
        if(packet.size%maxPacket==0)bulkOut(byteArrayOf())
        if(version>=2) { tx=(tx+1) and 65535;recent.addLast(packet);if(recent.size>96)recent.removeFirst() }
    }
    private fun readMux():Pair<Int,ByteArray>? {
        fun take():Pair<Int,ByteArray>? {
            if(incoming.size<8)return null
            val n=be32(incoming,4);val header=if(version<2)8 else 16
            // USB transfers are <=16 KiB, but a mux frame can span several transfers.
            require(n in header.toLong()..MAX_MUX_FRAME.toLong()) { "Invalid mux frame length=$n version=$version protocol=${be32(incoming,0)}" }
            if(incoming.size<n)return null
            val packet=incoming.copyOfRange(0,n.toInt());incoming=incoming.copyOfRange(n.toInt(),incoming.size)
            val proto=be32(packet,0).toInt()
            if(version>=2) {
                // 0xfeedface is emitted by the HOST. Neither the Swift reference nor
                // usbmuxd requires that value in the DEVICE's header. These mux
                // counters also aren't TCP byte sequences: repeated/skipped values
                // must not discard a new TCP payload. TCP handles duplicates below.
                // Echo offset 12 exactly as GuestUSB.swift @ 4bc3970 does.
                rx=be16(packet,12)
                // Offset 14 is the device's cumulative acknowledgement of HOST
                // mux packets (confirmed by the 0.2.4 phone headers). Never replay
                // the already accepted prefix: its duplicate replies can exhaust
                // the guest's endpoint-81 TRB queue. Match a retained sequence so a
                // stale acknowledgement cannot retire newer packets at wraparound.
                val acknowledged=be16(packet,14)
                val accepted=recent.indexOfFirst { be16(it,12)==acknowledged }
                if(accepted>=0) { repeat(accepted+1) { recent.removeFirst() };peerMuxAck=acknowledged }
            }
            if(muxTraceBudget>0) {
                muxTraceBudget--
                // Header metadata only: no restored plist, ticket, key or file data.
                val extra=if(version>=2)" reserved=0x${be32(packet,8).toString(16).padStart(8,'0')} seq12=$rx seq14=${be16(packet,14)}" else ""
                note("USB MUX RX v=$version proto=$proto bytes=$n$extra")
            }
            return proto to packet.copyOfRange(header,packet.size)
        }
        take()?.let { return it };drainIn();return take()
    }
    private fun resend(channel:Channel) {
        if(recent.isEmpty())return // A busy guest is not evidence of lost USB data.
        val patience=if(channel.seq!=channel.acked)3000 else 15000
        if(now()-progress<patience || now()-resent<patience)return
        resent=now();val packets=recent.toList()
        note("USB: повтор ${packets.size} неподтверждённых mux-пакетов; peerAck=$peerMuxAck first=${be16(packets.first(),12)} last=${be16(packets.last(),12)}")
        for(p in packets) {
            bulkOut(p);if(p.size%maxPacket==0)bulkOut(byteArrayOf())
            // Keep the opposite endpoint moving during recovery, not only after
            // OUT starts refusing. Buffer only: sending a new TCP ACK here would
            // overtake the old mux numbers still being replayed.
            drainIn()
        }
    }
    override fun connect(port:Int,timeoutMs:Long):GuestChannel {
        require(port in 1..65535 && channels.size<16)
        val channel=Channel(nextPort,port);nextPort=if(nextPort>=0xfffe)0xf001 else nextPort+1
        require(channels.put(channel.local,channel)==null)
        try {
            sendTcp(channel,2);val deadline=now()+timeoutMs
            while(now()<deadline) { service(5);if(channel.dead)throw IOException("Port $port refused");if(channel.established)return channel;resend(channel) }
            throw SocketTimeoutException("Port $port")
        } catch(e:Exception) { channel.close();throw e }
    }
    private fun sendTcp(c:Channel,flags:Int,payload:ByteArray=byteArrayOf()) {
        val h=ByteBuffer.allocate(20).order(ByteOrder.BIG_ENDIAN).putShort(c.local.toShort()).putShort(c.remote.toShort()).putInt(c.seq.toInt()).putInt(c.ack.toInt()).put(0x50).put(flags.toByte()).putShort(1024).putShort(0).putShort(0).array()
        sendMux(6,h+payload);c.seq=(c.seq+payload.size+(if(flags and 2 !=0)1 else 0)) and MASK
    }
    /** Returns immediately on an ACK; waiting out 200ms here throttles ASR. */
    fun service(timeoutMs:Long=5) {
        val deadline=now()+timeoutMs
        do {
            checkOpen();val packet=readMux()
            if(packet==null) { if(timeoutMs>0)Thread.sleep(1);continue }
            if(packet.first!=6)continue
            val p=packet.second;require(p.size>=20)
            val c=channels[be16(p,2)] ?: continue
            require(be16(p,0)==c.remote)
            val flags=p[13].toInt() and 255;val seq=be32(p,4);val acknowledged=be32(p,8)
            val h=(p[12].toInt() ushr 4 and 15)*4;require(h in 20..p.size)
            if(flags and 4 !=0) { c.dead=true;return }
            var staleAck=false
            if(flags and 16 !=0) {
                val delta=(acknowledged-c.acked) and MASK;val outstanding=(c.seq-c.acked) and MASK
                staleAck=delta>=0x80000000L
                if(!staleAck) {
                    require(delta<=outstanding) { "TCP acknowledgement outside sent window" }
                    if(delta>0)progress=now();c.acked=acknowledged
                }
            }
            if(!staleAck)c.window=be16(p,14) shl 8
            if(flags and 18 ==18 && !c.established) { c.ack=(seq+1) and MASK;c.established=true;sendTcp(c,16);return }
            val body=p.copyOfRange(h,p.size)
            if(body.isNotEmpty()) {
                if(seq==c.ack) {
                    require(c.inbox.size+body.size<=64*1024*1024) { "TCP input overflow" }
                    c.inbox+=body;c.ack=(c.ack+body.size) and MASK;progress=now()
                } else {
                    val behind=(c.ack-seq) and MASK
                    require(behind>=body.size && behind<0x80000000L) { "TCP sequence gap/overlap" }
                }
                sendTcp(c,16)
            }
            if(flags and 1 !=0) { c.ack=(c.ack+1) and MASK;sendTcp(c,16);c.dead=true }
            return
        } while(now()<deadline)
    }
    private inner class Channel(val local:Int,val remote:Int):GuestChannel {
        var seq=0L;var ack=0L;var acked=0L;var window=0;var established=false;var dead=false;var inbox=byteArrayOf()
        override fun write(bytes:ByteArray) {
            var at=0;var deadline=now()+120_000
            while(at<bytes.size) {
                checkOpen();if(dead)throw EOFException("TCP closed")
                val room=window-((seq-acked) and MASK)
                if(room<=0) { if(now()>deadline)throw SocketTimeoutException("TCP write window");service();resend(this);continue }
                var n=minOf((bytes.size-at).toLong(),(MTU-36).toLong(),room).toInt()
                // Avoid the controller's intermittent ZLP truncation.
                if((n+36)%maxPacket==0 && n>1)n--
                sendTcp(this,16,bytes.copyOfRange(at,at+n));at+=n;deadline=now()+120_000
                // The Swift client also has a background receive pump. This port
                // has one owner, so explicitly service IN between outgoing data
                // packets, even while the peer still advertises writable room.
                // Buffered replies from recovery do not mean the physical IN
                // endpoint is empty: drain it too before consuming that backlog.
                if(incoming.isNotEmpty())drainIn()
                service(0)
            }
        }
        override fun read(count:Int,timeoutMs:Long):ByteArray {
            require(count in 0..64*1024*1024);val deadline=now()+timeoutMs
            while(inbox.size<count) { checkOpen();if(dead)throw EOFException("TCP short read");if(now()>=deadline)throw SocketTimeoutException("TCP read");service();resend(this) }
            return inbox.copyOfRange(0,count).also { inbox=inbox.copyOfRange(count,inbox.size) }
        }
        override fun readAvailable(timeoutMs:Long):ByteArray {
            val deadline=now()+timeoutMs
            while(inbox.isEmpty()) { checkOpen();if(dead)throw EOFException("TCP closed");if(now()>=deadline)throw SocketTimeoutException("TCP read");service() }
            return inbox.also { inbox=byteArrayOf() }
        }
        override fun close() { channels.remove(local);if(!dead)runCatching { sendTcp(this,4) };dead=true }
    }
    override fun close() { closed=true;link.close() }
    companion object {
        const val MTU=16384;const val IN=0x69;const val OUT=0xe1;const val SETUP=0x2d;const val MASK=0xffffffffL
        private const val MAX_MUX_FRAME=4*1024*1024
        internal fun findMux(raw:ByteArray):Triple<Int,Int,Int>? {
            var at=0;var active=false;var input=0;var output=0;var packet=512
            while(at<raw.size) {
                require(at+2<=raw.size);val n=raw[at].toInt() and 255;require(n>=2 && at+n<=raw.size) { "USB descriptor length" }
                when(raw[at+1].toInt()) {
                    4 -> { if(active && input!=0 && output!=0)return Triple(input,output,packet);require(n>=9);active=raw[at+5].toInt()==-1 && raw[at+6].toInt()==-2 && raw[at+7].toInt()==2;input=0;output=0 }
                    5 -> if(active) { require(n>=7);if(raw[at+3].toInt() and 3==2) { val ep=raw[at+2].toInt() and 255;if(ep and 128 !=0)input=ep and 15 else output=ep and 15;packet=le16(raw,at+4);require(packet in 1..1024) } }
                }
                at+=n
            }
            return if(input!=0 && output!=0)Triple(input,output,packet) else null
        }
        internal fun be32(v:Int)=ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN).putInt(v).array()
        internal fun be32(b:ByteArray,p:Int)=ByteBuffer.wrap(b,p,4).order(ByteOrder.BIG_ENDIAN).int.toLong() and MASK
        internal fun be16(b:ByteArray,p:Int)=ByteBuffer.wrap(b,p,2).order(ByteOrder.BIG_ENDIAN).short.toInt() and 65535
        internal fun le16(b:ByteArray,p:Int)=ByteBuffer.wrap(b,p,2).order(ByteOrder.LITTLE_ENDIAN).short.toInt() and 65535
    }
}
