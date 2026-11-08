// SPDX-License-Identifier: GPL-3.0-or-later
// Kotlin port of SEPFirmware.swift @ 4bc3970.
package dev.embervm.android.restore

import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

object SepFirmware {
    class Key(val iv: ByteArray, val key: ByteArray) {
        init { require(iv.size==16 && key.size==32) }
        fun clear() { iv.fill(0);key.fill(0) }
        companion object {
            fun parse(text: String): Key {
                val clean=text.trim().removePrefix("0x").removePrefix("0X").filterNot { it.isWhitespace() }
                require(clean.length==96 && clean.all { it in "0123456789abcdefABCDEF" }) { "SEP: нужно 96 hex-цифр (IV + AES-256 key)" }
                val b=ByteArray(48) { clean.substring(it*2,it*2+2).toInt(16).toByte() }
                return Key(b.copyOfRange(0,16),b.copyOfRange(16,48)).also { b.fill(0) }
            }
        }
    }
    fun decrypt(payload: ByteArray, key: Key): ByteArray {
        require(payload.isNotEmpty() && payload.size%16==0) { "SEP payload must be block sized" }
        return Cipher.getInstance("AES/CBC/NoPadding").run { init(Cipher.DECRYPT_MODE,SecretKeySpec(key.key,"AES"),IvParameterSpec(key.iv));doFinal(payload) }
    }
    fun rebuild(encrypted: ByteArray, key: Key, ticket: ByteArray): ByteArray = Img4.make(Img4.Payload("rsep","none",decrypt(Img4.readIM4P(encrypted).data,key)),ticket)
}
