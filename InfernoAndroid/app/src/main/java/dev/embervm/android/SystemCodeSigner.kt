// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

/** Ad-hoc signing for the copied guest executable, not real-device provisioning.
 * Run preparation in its own service process; signing is serialized. */
object SystemCodeSigner {
    init { System.loadLibrary("ember_codesign") }
    @JvmStatic private external fun nativeSign(image:ByteArray,entitlements:ByteArray,identifier:String,flags:Int,platform:Int):ByteArray
    @Synchronized fun sign(prepared:SystemMachO.Prepared):ByteArray {
        val identity=prepared.identity
        val result=nativeSign(prepared.bytes,identity.entitlements,identity.identifier,identity.flags,identity.platform)
        SystemCodeSignature.verify(result)
        val check=SystemMachO.inspect(result)
        require(check.uuid==identity.uuid && check.cpuSubtype==identity.cpuSubtype && check.identifier==identity.identifier && check.platform==identity.platform && check.flags==(identity.flags or 2) &&
            check.dylibs.contains(prepared.dependency) && check.entitlements.contentEquals(identity.entitlements)) { "Signing changed executable identity or entitlements" }
        return result
    }
}
