// SPDX-License-Identifier: GPL-3.0-or-later
// Kotlin port of IMG4.swift @ 4bc3970.
package dev.embervm.android.restore

object Img4 {
    data class Payload(val type: String, val version: String, val data: ByteArray, val extras: List<Der.Node> = emptyList())
    fun readIM4P(bytes: ByteArray): Payload {
        val c = Der.parse(bytes).children ?: error("IM4P: missing sequence")
        require(c.size >= 4 && c[0].text == "IM4P" && c[3].identifier.contentEquals(byteArrayOf(4))) { "Not an IM4P" }
        return Payload(c[1].text ?: error("IM4P type"), c[2].text ?: error("IM4P version"), c[3].value, c.drop(4))
    }
    fun makeIM4P(type: String, version: String, data: ByteArray) = Der.encode(payloadNode(Payload(type,version,data)))
    private fun payloadNode(p: Payload) = Der.sequence(listOf(Der.ia5("IM4P"),Der.ia5(p.type),Der.ia5(p.version),Der.octets(p.data)) + p.extras)
    fun make(im4p: ByteArray, ticket: ByteArray) = make(readIM4P(im4p),ticket)
    fun make(payload: Payload, ticket: ByteArray): ByteArray {
        val t = Der.parse(ticket)
        require(t.children?.firstOrNull()?.text == "IM4M") { "Not an IM4M ticket" }
        return Der.encode(Der.sequence(listOf(Der.ia5("IMG4"),payloadNode(payload),Der.Node(byteArrayOf(0xa0.toByte()),children=mutableListOf(t)))))
    }
}
