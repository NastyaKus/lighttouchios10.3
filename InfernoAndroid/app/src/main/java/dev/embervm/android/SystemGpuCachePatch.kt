// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.RandomAccessFile
import java.security.MessageDigest
import java.nio.ByteBuffer
import java.util.UUID

/** Prepare only the main EAGL renderer's two original instructions from the
 * user's own Inferno backup. Other software/no-Metal patches remain enabled.
 * This component NEVER writes a cache or enables the unfinished system driver. */
object SystemGpuCachePatch {
    const val CACHE_SIZE=2052489216L
    const val CACHE_UUID="11ad39c9-e3c9-3067-a337-7287744fdcb5"
    data class Write(val offset:Long,val expectedSoftware:ByteArray,val original:ByteArray)
    data class Prepared(val writes:List<Write>)
    enum class State { SOFTWARE, MAIN_EAGL_RESTORED, PARTIAL }
    private fun hex(text:String)=ByteArray(text.length/2) { text.substring(it*2,it*2+2).toInt(16).toByte() }
    private fun sha(bytes:ByteArray)=MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    internal fun originals(backup:ByteArray,sites:List<SystemGpuCachePlan.Site>):Map<Long,ByteArray> {
        require(backup.size in 1..65536 && backup.all { it.toInt() in 9..13 || it.toInt() in 32..126 }) { "Invalid Inferno original-byte backup" }
        val expected=sites.flatMap { site -> (0 until site.software.length/2).map { site.offset+it } }.toSet()
        require(expected.size==sites.sumOf { it.software.length/2 }) { "Overlapping patch plan" }
        val bytes=mutableMapOf<Long,Byte>();var cursor=-1L
        val tokens=backup.toString(Charsets.US_ASCII).trim().split(Regex("\\s+"))
        for(token in tokens) {
            if(token.matches(Regex("[0-9a-fA-F]{1,16}:"))) {
                cursor=token.dropLast(1).toLongOrNull(16) ?: error("Original-byte offset overflow")
                require(cursor in expected) { "Unexpected original-byte offset" }
            } else {
                require(token.matches(Regex("[0-9a-fA-F]{1,2}")) && cursor in expected && cursor !in bytes) { "Invalid/duplicate original byte" }
                bytes[cursor]=token.toInt(16).toByte();cursor++
            }
        }
        require(bytes.keys==expected) { "Incomplete original-byte backup" }
        return sites.associate { site ->
            val original=ByteArray(site.software.length/2) { bytes.getValue(site.offset+it) }
            require(sha(original)==site.originalSha256) { "Original-byte SHA-256 does not match 18A5351d at ${site.offset.toString(16)}" }
            site.offset to original
        }
    }
    private fun identity(cache:RandomAccessFile) {
        require(cache.length()==CACHE_SIZE) { "Unexpected dyld cache length" }
        val header=ByteArray(104);cache.seek(0);cache.readFully(header)
        require(header.copyOfRange(0,16).contentEquals("dyld_v1  arm64e\u0000".toByteArray(Charsets.US_ASCII)))
        val uuid=ByteBuffer.wrap(header,88,16)
        require(UUID(uuid.long,uuid.long).toString()==CACHE_UUID) { "Unexpected dyld cache UUID" }
    }
    internal fun inspect(cache:RandomAccessFile,sites:List<SystemGpuCachePlan.Site>,original:Map<Long,ByteArray>):State {
        var software=0;var restored=0;var selected=0
        for(site in sites) {
            val bytes=ByteArray(site.software.length/2);cache.seek(site.offset);cache.readFully(bytes)
            val replacement=hex(site.software)
            if(site.restore) {
                selected++
                if(bytes.contentEquals(replacement))software++
                else if(bytes.contentEquals(original.getValue(site.offset)))restored++
                else error("Unknown renderer patch bytes")
            } else require(bytes.contentEquals(replacement)) { "Unrelated software patch changed at ${site.offset.toString(16)}" }
        }
        require(selected>0)
        return when { software==selected->State.SOFTWARE;restored==selected->State.MAIN_EAGL_RESTORED;else->State.PARTIAL }
    }
    fun prepare(cache:RandomAccessFile,backup:ByteArray):Prepared {
        identity(cache)
        val sites=SystemGpuCachePlan.sites;val original=originals(backup,sites)
        require(inspect(cache,sites,original)==State.SOFTWARE) { "EAGL renderer is already modified or incompletely restored" }
        return Prepared(sites.filter { it.restore }.map { Write(it.offset,hex(it.software),original.getValue(it.offset)) })
    }
    fun verify(cache:RandomAccessFile,backup:ByteArray):State {
        identity(cache);return inspect(cache,SystemGpuCachePlan.sites,originals(backup,SystemGpuCachePlan.sites))
    }
}
