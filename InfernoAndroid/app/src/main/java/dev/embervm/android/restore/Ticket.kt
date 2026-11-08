// SPDX-License-Identifier: GPL-3.0-or-later
// Kotlin port of Ticket.swift @ 4bc3970; modified tickets are emulator-only.
package dev.embervm.android.restore

import dev.embervm.android.Plist

object Ticket {
    const val ECID = 0x1122334455667788L
    val apComponents = linkedMapOf("rosi" to "OS","krnl" to "KernelCache","dtre" to "DeviceTree","rdtr" to "RestoreDeviceTree","trst" to "StaticTrustCache","rtsc" to "RestoreTrustCache","mtfw" to "Multitouch","anef" to "ANE","aopf" to "AOP","avef" to "AVE","hpas" to "Ap,HapticAssets","acfw" to "AudioCodecFirmware","gfxf" to "GFX","ispf" to "ISP","lphp" to "LeapHaptics","pmpf" to "PMP","siof" to "SIO","sepi" to "SEP","rsep" to "RestoreSEP","rdsk" to "RestoreRamDisk","isys" to "SystemVolume","msys" to "SystemVolumeCanonicalMetadata")
    val sepComponents = apComponents.filterKeys { it in setOf("rosi","krnl","dtre","rdtr","trst","rtsc","sepi","rsep") }
    private val corrupted = setOf("rfta","ftap","rfts","ftsp")
    fun forgeAP(shsh: ByteArray, identity: Map<String,Any?>): ByteArray = root(shsh).let { t -> rewrite(components(t),identity,apComponents,null); Der.encode(t) }
    fun forgeSEP(shsh: ByteArray, identity: Map<String,Any?>, chipID: Long = 0x8030): ByteArray {
        val t = root(shsh); rewrite(components(t),identity,sepComponents,chipID)
        val fields = t.children?.getOrNull(4)?.children?.firstOrNull()?.children?.firstOrNull()?.children ?: error("Ticket: certificate")
        val extensions = fields.firstOrNull { it.identifier.contentEquals(byteArrayOf(0xa3.toByte())) }?.children?.firstOrNull()?.children ?: error("Ticket: extensions")
        val value = extensions.getOrNull(4)?.children?.lastOrNull() ?: error("Ticket: certificate manifest extension")
        require(value.identifier.contentEquals(byteArrayOf(4)))
        val inner = Der.parse(value.value); rewrite(inner.children ?: error("Ticket: manifest SET"),identity,sepComponents,chipID,false)
        value.value = Der.encode(inner)
        return Der.encode(t)
    }
    private fun root(shsh: ByteArray): Der.Node {
        val dict = Plist.parse(shsh.inputStream()) as? Map<*,*> ?: error("ticket.shsh2 is not an XML plist")
        val bytes = dict["ApImg4Ticket"] as? ByteArray ?: error("ticket.shsh2 has no ApImg4Ticket data")
        return Der.parse(bytes).also { require(it.children?.firstOrNull()?.text == "IM4M") { "Not an IM4M" } }
    }
    private fun components(t: Der.Node): List<Der.Node> = t.children?.getOrNull(2)?.children?.firstOrNull()?.children?.firstOrNull()?.children?.getOrNull(1)?.children ?: error("Ticket: MANB shape")
    private fun field(name: String, component: Der.Node): Der.Node? = component.children?.firstOrNull()?.children?.getOrNull(1)?.children?.firstOrNull { it.fourCC == name }?.children?.firstOrNull()?.children?.getOrNull(1)
    private fun rewrite(nodes: List<Der.Node>, identity: Map<String,Any?>, names: Map<String,String>, chip: Long?, touchIdentity: Boolean = true) {
        val manifest = identity["Manifest"] as? Map<*,*> ?: error("Missing Manifest")
        for (node in nodes) {
            val code = node.fourCC ?: continue
            if (code in names) {
                val digest = (manifest[names[code]] as? Map<*,*>)?.get("Digest") as? ByteArray
                val f = field("DGST",node)
                if (digest != null && f != null) { f.identifier=byteArrayOf(4);f.value=digest }
            } else if (code in corrupted) {
                node.children?.firstOrNull()?.children?.firstOrNull()?.let { it.value=it.value.reversedArray() }
            } else if (code=="MANP" && chip!=null) {
                field("CHIP",node)?.let { it.identifier=byteArrayOf(2);it.value=Der.integer(chip).value }
                if (touchIdentity) {
                    field("ECID",node)?.let { it.identifier=byteArrayOf(2);it.value=Der.integer(ECID).value }
                    field("snon",node)?.let { it.identifier=byteArrayOf(4);it.value=ByteArray(20) { i -> byteArrayOf(0xfe.toByte(),0xed.toByte(),0xfa.toByte(),0xce.toByte())[i%4] } }
                }
            }
        }
    }
}
