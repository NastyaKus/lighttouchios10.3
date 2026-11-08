// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Test

class SystemMachOTest {
    private val uuid="00010203-0405-0607-0809-0a0b0c0d0e0f"
    private val path="/usr/lib/ember/libEmberSystemGLES.dylib"
    private fun digest(bytes:ByteArray)=MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
    private fun fixture():ByteArray {
        val bytes=ByteArray(1024);val b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(0,0xfeedfacf.toInt());b.putInt(4,0x100000c);b.putInt(8,0x80000002.toInt());b.putInt(12,2);b.putInt(16,3);b.putInt(20,192)
        b.putInt(32,0x19);b.putInt(36,152);b.putLong(32+48,512);b.putInt(32+64,1)
        b.putLong(32+72+40,16);b.putInt(32+72+48,512)
        b.putInt(184,0x1b);b.putInt(188,24);repeat(16){bytes[192+it]=it.toByte()}
        b.putInt(208,0x1d);b.putInt(212,16);b.putInt(216,768);b.putInt(220,256)
        val s=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        s.putInt(768,0xfade0cc0.toInt());s.putInt(772,256);s.putInt(776,2)
        s.putInt(780,0);s.putInt(784,32);s.putInt(788,5);s.putInt(792,160)
        s.putInt(800,0xfade0c02.toInt());s.putInt(804,128);s.putInt(808,0x20400);s.putInt(812,2);s.putInt(820,64)
        "com.example.synthetic\u0000".toByteArray().copyInto(bytes,864)
        val xml="<plist><dict/></plist>".toByteArray();s.putInt(928,0xfade7171.toInt());s.putInt(932,8+xml.size);xml.copyInto(bytes,936)
        return bytes
    }
    @Test fun insertsOnlyIntoVerifiedPaddingAndPreservesIdentityAndEntitlements() {
        val input=fixture();val before=input.copyOf();val identity=SystemMachO.inspect(input)
        assertEquals(uuid,identity.uuid);assertEquals("com.example.synthetic",identity.identifier)
        val prepared=SystemMachO.inject(input,path,uuid,digest(input));val result=SystemMachO.inspect(prepared.bytes)
        assertArrayEquals(before,input);assertEquals(4,result.commands);assertEquals(listOf(path),result.dylibs)
        assertArrayEquals(identity.entitlements,result.entitlements)
        assertArrayEquals(input.copyOfRange(512,input.size),prepared.bytes.copyOfRange(512,input.size))
    }
    @Test fun refusesWrongIdentityNonzeroPaddingOrUnsupportedInputWithoutMutating() {
        val input=fixture();val before=input.copyOf()
        assertTrue(runCatching { SystemMachO.inject(input,path,"00000000-0000-0000-0000-000000000000",digest(input)) }.isFailure)
        assertTrue(runCatching { SystemMachO.inject(input,path,uuid,"0".repeat(64)) }.isFailure)
        assertTrue(runCatching { SystemMachO.inject(input,"/usr/lib/ember/../escape.dylib",uuid,digest(input)) }.isFailure)
        val changed=input.copyOf();changed[225]=1
        assertTrue(runCatching { SystemMachO.inject(changed,path,uuid,digest(changed)) }.isFailure)
        assertTrue(runCatching { SystemMachO.inspect(input.copyOfRange(0,24)) }.isFailure)
        assertArrayEquals(before,input)
    }
    @Test fun refusesMalformedCommandsSignaturesAndDuplicateInjection() {
        for((offset,value) in listOf(16 to Int.MAX_VALUE,20 to Int.MAX_VALUE,36 to 0,36 to 151,216 to Int.MAX_VALUE,32+72+48 to 1)) {
            val bytes=fixture();ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(offset,value)
            assertTrue("offset=$offset",runCatching { SystemMachO.inspect(bytes) }.isFailure)
        }
        val input=fixture();val injected=SystemMachO.inject(input,path,uuid,digest(input)).bytes
        assertTrue(runCatching { SystemMachO.inject(injected,path,uuid,digest(injected)) }.isFailure)
    }
}
