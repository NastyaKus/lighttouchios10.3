// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

/** File preparation only. The output has not been installed or executed as
 * backboardd, and does not constitute a working system GPU frontend. */
object SystemGpuPreparation {
    const val BACKBOARDD_UUID="145adb09-0b51-3ef4-8a1c-59cb046cbc5f"
    const val BACKBOARDD_SHA256="304d45d72dcba7e16928ec9192897c269280846f6d06dcccfdb4b991cbf040d2"
    const val LIBRARY="/usr/lib/ember/libEmberSystemGLES.dylib"
    fun backboardd(build:String,stockExecutable:ByteArray):ByteArray {
        require(build==SystemGpuProfile.BUILD) { "Only 18A5351d system ABI is supported" }
        val prepared=SystemMachO.inject(stockExecutable,LIBRARY,BACKBOARDD_UUID,BACKBOARDD_SHA256)
        // Verify extraction against the original embedded hashes before a new
        // signature could make damaged input appear internally consistent.
        SystemCodeSignature.verify(stockExecutable)
        return SystemCodeSigner.sign(prepared)
    }
}
