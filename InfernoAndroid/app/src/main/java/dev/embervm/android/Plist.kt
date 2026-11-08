package dev.embervm.android

import org.xmlpull.v1.XmlPullParser
import org.xmlpull.v1.XmlPullParserFactory
import java.io.File
import java.io.InputStream
import java.util.Base64

/** XML plist reader shared by the launcher and restore protocol. Binary plists are unsupported. */
object Plist {
    fun parse(input: InputStream): Any? {
        val p=XmlPullParserFactory.newInstance().newPullParser()
        p.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES,false)
        p.setInput(input,null)
        while (p.next()!=XmlPullParser.START_TAG) require(p.eventType!=XmlPullParser.END_DOCUMENT) { "Empty plist" }
        require(p.name=="plist") { "Expected plist root" }
        while (p.next()!=XmlPullParser.START_TAG) require(p.eventType!=XmlPullParser.END_DOCUMENT && p.eventType!=XmlPullParser.END_TAG) { "Empty plist value" }
        val result=value(p)
        while(p.next()!=XmlPullParser.END_TAG) require(p.eventType!=XmlPullParser.END_DOCUMENT && p.eventType!=XmlPullParser.START_TAG) { "Multiple plist values" }
        require(p.name=="plist")
        while(p.next()!=XmlPullParser.END_DOCUMENT) require(p.eventType!=XmlPullParser.START_TAG) { "Trailing XML" }
        return result
    }
    fun parse(file: File): Any? = runCatching { file.inputStream().buffered().use { parse(it) } }.getOrNull()
    private fun next(p: XmlPullParser): Int = p.next().also { require(it!=XmlPullParser.END_DOCUMENT) { "Truncated XML plist" } }
    private fun value(p: XmlPullParser): Any? {
        require(p.depth<=128) { "Plist nesting too deep" }
        return when(p.name) {
        "dict" -> {
            val map=linkedMapOf<String,Any?>();var key: String?=null
            while (next(p)!=XmlPullParser.END_TAG) {
                if (p.eventType!=XmlPullParser.START_TAG) continue
                if (p.name=="key") { require(key==null);key=p.nextText() }
                else { val k=requireNotNull(key) { "Plist value without key" };require(!map.containsKey(k)) { "Duplicate plist key" };map[k]=value(p);key=null }
            }
            require(key==null) { "Plist key without value" };map
        }
        "array" -> buildList { while(next(p)!=XmlPullParser.END_TAG) if(p.eventType==XmlPullParser.START_TAG) add(value(p)) }
        "string","date" -> p.nextText()
        "data" -> Base64.getDecoder().decode(p.nextText().filterNot { it.isWhitespace() })
        "integer" -> p.nextText().trim().let { if(it.startsWith("0x")) java.math.BigInteger(it.drop(2),16).let { n -> if(n.bitLength()<64)n.toLong() else n } else it.toLongOrNull() ?: java.math.BigInteger(it) }
        "real" -> p.nextText().trim().toDouble()
        "true" -> { require(p.nextText().isBlank());true }
        "false" -> { require(p.nextText().isBlank());false }
        else -> error("Unknown plist type: ${p.name}")
    }
    }
}
