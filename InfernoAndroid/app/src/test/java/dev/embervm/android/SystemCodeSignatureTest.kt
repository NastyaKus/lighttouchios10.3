// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Test

/** Real hash relationships, synthetic executable: no Apple binaries in tests. */
class SystemCodeSignatureTest {
    private fun fixture():ByteArray {
        val bytes=ByteArray(4096+512)
        val b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(0,0xfeedfacf.toInt());b.putInt(4,0x100000c);b.putInt(8,2);b.putInt(12,2)
        b.putInt(16,3);b.putInt(20,192)
        b.putInt(32,0x19);b.putInt(36,152);b.putLong(32+48,4096);b.putInt(32+64,1)
        "__info_plist".toByteArray().copyInto(bytes,104);"__TEXT".toByteArray().copyInto(bytes,120)
        b.putLong(104+40,32);b.putInt(104+48,512)
        "synthetic embedded info".toByteArray().copyInto(bytes,512)
        b.putInt(184,0x1b);b.putInt(188,24);repeat(16){bytes[192+it]=it.toByte()}
        b.putInt(208,0x1d);b.putInt(212,16);b.putInt(216,4096);b.putInt(220,512)
        val s=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        s.putInt(4096,0xfade0cc0.toInt());s.putInt(4100,512);s.putInt(4104,2)
        s.putInt(4108,0);s.putInt(4112,32);s.putInt(4116,5);s.putInt(4120,400)
        val cd=4128;val hashes=256
        s.putInt(cd,0xfade0c02.toInt());s.putInt(cd+4,320);s.putInt(cd+8,0x20100);s.putInt(cd+12,2)
        s.putInt(cd+16,hashes);s.putInt(cd+20,48);s.putInt(cd+24,5);s.putInt(cd+28,1);s.putInt(cd+32,4096)
        bytes[cd+36]=32;bytes[cd+37]=2;bytes[cd+39]=12
        "dev.ember.synthetic\u0000".toByteArray().copyInto(bytes,cd+48)
        val ent=4496;val xml="<plist><dict><key>test</key><true/></dict></plist>".toByteArray()
        s.putInt(ent,0xfade7171.toInt());s.putInt(ent+4,8+xml.size);xml.copyInto(bytes,ent+8)
        fun hash(from:Int,size:Int,to:Int) {
            MessageDigest.getInstance("SHA-256").digest(bytes.copyOfRange(from,from+size)).copyInto(bytes,to)
        }
        hash(512,32,cd+hashes-32);hash(ent,8+xml.size,cd+hashes-5*32)
        hash(0,4096,cd+hashes)
        return bytes
    }
    @Test fun verifiesCodePagesEmbeddedInfoAndEntitlements() { SystemCodeSignature.verify(fixture()) }
    @Test fun refusesTamperedCodeInfoEntitlementsAndHashes() {
        for(at in listOf(600,512,4508,4384,4352)) {
            val bytes=fixture();bytes[at]=(bytes[at].toInt() xor 1).toByte()
            assertTrue("tampered $at",runCatching { SystemCodeSignature.verify(bytes) }.isFailure)
        }
    }
    @Test fun refusesIncompleteCoverageScatterAndUnknownAlgorithm() {
        for((at,value) in listOf(4128+12 to 0,4128+28 to 2,4128+32 to 4095,4128+44 to 48)) {
            val bytes=fixture();ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN).putInt(at,value)
            assertTrue("invalid $at",runCatching { SystemCodeSignature.verify(bytes) }.isFailure)
        }
        val bytes=fixture();bytes[4128+37]=99
        assertTrue(runCatching { SystemCodeSignature.verify(bytes) }.isFailure)
    }
}
