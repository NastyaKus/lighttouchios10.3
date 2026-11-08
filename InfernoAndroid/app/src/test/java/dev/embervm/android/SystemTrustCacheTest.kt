// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import dev.embervm.android.restore.Img4
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Test

class SystemTrustCacheTest {
    private fun entry(first:Int)=SystemTrustCache.Entry(ByteArray(20).also { it[0]=first.toByte() },2)
    @Test fun sortsAsUnsignedPreservesOldEntriesAndOnlyAddsSpecificHashes() {
        val stock=SystemTrustCache.encode(SystemTrustCache.Cache(ByteArray(16){it.toByte()},listOf(entry(255),entry(1))))
        val im4p=Img4.makeIM4P("rtsc","1",stock)
        val augmented=SystemTrustCache.augmentIM4P(im4p,listOf(entry(128)))
        val decoded=SystemTrustCache.decode(Img4.readIM4P(augmented).data)
        assertEquals(listOf(1,128,255),decoded.entries.map { it.hash[0].toInt() and 255 })
        assertFalse(decoded.uuid.contentEquals(ByteArray(16){it.toByte()}))
        assertArrayEquals(augmented,SystemTrustCache.augmentIM4P(augmented,listOf(entry(128))))
        assertArrayEquals(stock,Img4.readIM4P(im4p).data)
    }
    @Test fun rejectsWrongVersionTruncationUnsortedAndConflictingFlagsButPreservesStockStyleDuplicate() {
        val stock=SystemTrustCache.encode(SystemTrustCache.Cache(ByteArray(16),listOf(entry(1),entry(2))))
        for(at in listOf(0,20)) {
            val bytes=stock.copyOf();bytes[at]=99
            assertTrue(runCatching { SystemTrustCache.decode(bytes) }.isFailure)
        }
        for(size in stock.indices)assertTrue(runCatching { SystemTrustCache.decode(stock.copyOf(size)) }.isFailure)
        val swapped=stock.copyOf();stock.copyOfRange(46,68).copyInto(swapped,24);stock.copyOfRange(24,46).copyInto(swapped,46)
        assertTrue(runCatching { SystemTrustCache.decode(swapped) }.isFailure)
        val wrapped=Img4.makeIM4P("rtsc","1",stock)
        assertTrue(runCatching { SystemTrustCache.augmentIM4P(wrapped,listOf(entry(1).copy(flags=1))) }.isFailure)
        val duplicate=SystemTrustCache.encode(SystemTrustCache.Cache(ByteArray(16),listOf(entry(1),entry(1))))
        assertEquals(2,SystemTrustCache.decode(duplicate).entries.size)
        assertTrue(runCatching { SystemTrustCache.encode(SystemTrustCache.Cache(ByteArray(16),listOf(entry(1),entry(1).copy(flags=1)))) }.isFailure)
    }
    @Test fun cdhashIsHashOfCodeDirectoryRatherThanWholeExecutable() {
        val bytes=ByteArray(512);val b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(0,0xfeedfacf.toInt());b.putInt(4,0x100000c);b.putInt(12,6);b.putInt(16,1);b.putInt(20,16)
        b.putInt(32,0x1d);b.putInt(36,16);b.putInt(40,256);b.putInt(44,256)
        val s=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        s.putInt(256,0xfade0cc0.toInt());s.putInt(260,88);s.putInt(264,1);s.putInt(268,0);s.putInt(272,24)
        s.putInt(280,0xfade0c02.toInt());s.putInt(284,64);bytes[280+37]=2
        val expected=MessageDigest.getInstance("SHA-256").digest(bytes.copyOfRange(280,344)).copyOfRange(0,20)
        val hash=SystemTrustCache.codeDirectoryHashes(bytes).single();assertArrayEquals(expected,hash.hash);assertEquals(2,hash.hashType)
        bytes[128]=1;assertArrayEquals(hash.hash,SystemTrustCache.codeDirectoryHashes(bytes).single().hash)
        // Page corruption must be caught by the signature verifier, not disguised
        // as cdhash verification; extraction alone intentionally cannot detect it.
        s.putInt(272,Int.MAX_VALUE);assertTrue(runCatching { SystemTrustCache.codeDirectoryHashes(bytes) }.isFailure)
    }
}
