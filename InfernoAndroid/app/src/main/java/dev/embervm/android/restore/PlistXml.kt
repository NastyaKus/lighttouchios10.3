// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android.restore

import java.time.Instant
import java.util.Base64
import java.util.Date

object PlistXml {
    fun encode(value: Any?): ByteArray = buildString {
        append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">")
        emit(value);append("</plist>\n")
    }.toByteArray(Charsets.UTF_8)
    private fun StringBuilder.text(s: String) {
        s.forEach { c -> require(c=='\t'||c=='\n'||c=='\r'||c.code>=32) { "Invalid XML control character" };append(when(c){'&'->"&amp;";'<'->"&lt;";'>'->"&gt;";'\"'->"&quot;";'\''->"&apos;";else->c.toString()}) }
    }
    private fun StringBuilder.tag(name: String, body: String) { append('<').append(name).append('>');text(body);append("</").append(name).append('>') }
    private fun StringBuilder.emit(v: Any?) {
        when(v) {
            is Map<*,*> -> { append("<dict>");v.forEach { (k,x)->require(k is String);tag("key",k);emit(x) };append("</dict>") }
            is ByteArray -> tag("data",Base64.getEncoder().encodeToString(v))
            is String -> tag("string",v)
            is Boolean -> append(if(v) "<true/>" else "<false/>")
            is Byte,is Short,is Int,is Long,is java.math.BigInteger -> tag("integer",v.toString())
            is Float -> { require(v.isFinite());tag("real",v.toString()) }
            is Double -> { require(v.isFinite());tag("real",v.toString()) }
            is Instant -> tag("date",v.toString())
            is Date -> tag("date",v.toInstant().toString())
            is Iterable<*> -> { append("<array>");v.forEach { emit(it) };append("</array>") }
            is Array<*> -> { append("<array>");v.forEach { emit(it) };append("</array>") }
            else -> error("Unsupported plist value ${v?.javaClass?.simpleName ?: "null"}")
        }
    }
}
