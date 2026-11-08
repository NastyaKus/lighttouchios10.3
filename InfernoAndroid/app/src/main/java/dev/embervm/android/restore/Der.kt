// SPDX-License-Identifier: GPL-3.0-or-later
// Kotlin port of IMG4.swift, MakrSas/Inferno-iOS @ 4bc3970.
package dev.embervm.android.restore

import java.io.ByteArrayOutputStream
import java.math.BigInteger

object Der {
    class Node(var identifier: ByteArray, var value: ByteArray = byteArrayOf(), var children: MutableList<Node>? = null) {
        val tagClass get() = identifier[0].toInt() and 0xc0
        val tagNumber: Long get() {
            if (identifier[0].toInt() and 31 != 31) return (identifier[0].toInt() and 31).toLong()
            return identifier.drop(1).fold(0L) { n, b -> (n shl 7) or (b.toLong() and 127) }
        }
        val fourCC: String? get() {
            if (tagClass != 0xc0 || tagNumber <= 0xffffff || tagNumber > 0xffffffffL) return null
            val b = ByteArray(4) { (tagNumber ushr (8 * (3-it))).toByte() }
            return if (b.all { (it.toInt() and 255) in 32..126 }) String(b, Charsets.US_ASCII) else null
        }
        val text: String? get() = if (children == null) String(value, Charsets.UTF_8) else null
        fun child(name: String) = children?.firstOrNull { it.fourCC == name }
    }
    private class Reader(val bytes: ByteArray) {
        var at = 0
        fun node(end: Int, depth: Int): Node {
            require(depth < 128 && at < end) { "DER: truncated or too deeply nested" }
            val start = at++; val first = bytes[start].toInt() and 255
            if (first and 31 == 31) {
                var groups = 0; var number = 0L
                do {
                    require(at < end && groups++ < 9) { "DER: invalid long tag" }
                    val b = bytes[at++].toInt() and 255
                    require(number <= Long.MAX_VALUE ushr 7) { "DER: tag overflow" }
                    number = (number shl 7) or (b and 127).toLong()
                    if (b and 128 == 0) break
                } while (true)
            }
            val id = bytes.copyOfRange(start, at)
            require(at < end) { "DER: missing length" }
            var length = (bytes[at++].toInt() and 255).toLong()
            if (length and 128 != 0L) {
                val count = (length and 127).toInt()
                require(count in 1..4 && count <= end-at) { "DER: indefinite/oversized length" }
                length = 0
                repeat(count) { length = (length shl 8) or (bytes[at++].toLong() and 255) }
            }
            require(length <= end-at) { "DER: length exceeds input" }
            val limit = at + length.toInt()
            return if (first and 32 != 0) {
                val children = mutableListOf<Node>()
                while (at < limit) children += node(limit, depth+1)
                Node(id, children = children)
            } else Node(id, bytes.copyOfRange(at, limit)).also { at = limit }
        }
    }
    fun parse(bytes: ByteArray): Node = Reader(bytes).run {
        val result = node(bytes.size, 0)
        require(at == bytes.size) { "DER: trailing bytes" }; result
    }
    fun parseAll(bytes: ByteArray): List<Node> = Reader(bytes).run {
        buildList { while (at < bytes.size) add(node(bytes.size, 0)) }
    }
    fun encode(node: Node): ByteArray {
        val body = node.children?.let { nodes -> ByteArrayOutputStream().apply { nodes.forEach { write(encode(it)) } }.toByteArray() } ?: node.value
        val len = if (body.size < 128) byteArrayOf(body.size.toByte()) else {
            var n = body.size; val b = mutableListOf<Byte>()
            while (n > 0) { b.add(0, n.toByte()); n = n ushr 8 }
            byteArrayOf((128 or b.size).toByte()) + b.toByteArray()
        }
        return node.identifier + len + body
    }
    fun sequence(nodes: List<Node>) = Node(byteArrayOf(0x30), children=nodes.toMutableList())
    fun set(nodes: List<Node>) = Node(byteArrayOf(0x31), children=nodes.toMutableList())
    fun ia5(text: String) = Node(byteArrayOf(0x16), text.toByteArray(Charsets.US_ASCII))
    fun octets(bytes: ByteArray) = Node(byteArrayOf(4), bytes)
    fun integer(value: Long) = integer(BigInteger.valueOf(value))
    fun integer(value: BigInteger): Node { require(value.signum() >= 0); return Node(byteArrayOf(2), value.toByteArray()) }
    fun tagged(name: String, nodes: List<Node>): Node {
        require(name.length == 4 && name.all { it.code in 32..126 })
        var number = name.fold(0L) { n, c -> (n shl 8) or c.code.toLong() }
        val groups = mutableListOf<Int>()
        do { groups.add(0, (number and 127).toInt()); number = number ushr 7 } while (number > 0)
        return Node(byteArrayOf(0xff.toByte()) + groups.mapIndexed { i,b -> (b or if (i < groups.lastIndex) 128 else 0).toByte() }.toByteArray(), children=mutableListOf(sequence(nodes)))
    }
}
