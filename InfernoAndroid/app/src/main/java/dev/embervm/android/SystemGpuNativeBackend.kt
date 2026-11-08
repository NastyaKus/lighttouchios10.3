// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Real host GLES3 execution. The guest GLI frontend is a separate dependency;
 * the existence of this backend does not enable or validate system GPU in iOS.
 * Construct/use/close only on the endpoint's worker in the isolated process. */
internal class SystemGpuNativeBackend: SystemGpuEndpoint.Backend {
    private val owner=Thread.currentThread()
    private var token=nativeOpen().also { check(it!=0L) }
    override val capabilities:SystemGpuEndpoint.Capabilities
    init {
        try {
            val info=nativeInfo(token)
            check(info.size==2)
            capabilities=SystemGpuEndpoint.Capabilities(3,0,info[0].toInt(),info[1])
        } catch(e:Throwable) { nativeClose(token);token=0;throw e }
    }
    private fun live():Long {
        check(Thread.currentThread()===owner && token!=0L) { "GPU backend owner/lifetime mismatch" }
        return token
    }
    override fun create(context:Long,share:Long,width:Int,height:Int)=nativeCreate(live(),context,share,width,height)
    override fun destroy(context:Long)=nativeDestroy(live(),context)
    override fun call(context:Long,command:ByteArray,upload:ByteArray?)=nativeCall(live(),context,command,upload)
    override fun present(context:Long,surface:Long,serial:Long)=ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
        .putInt(nativePresent(live(),context,surface,serial)).array()
    override fun close() { if(token!=0L){nativeClose(live());token=0} }
    companion object {
        init { System.loadLibrary("ember_system_gpu") }
        @JvmStatic private external fun nativeOpen():Long
        @JvmStatic private external fun nativeInfo(token:Long):Array<String>
        @JvmStatic private external fun nativeCreate(token:Long,context:Long,share:Long,width:Int,height:Int)
        @JvmStatic private external fun nativeDestroy(token:Long,context:Long)
        @JvmStatic private external fun nativeCall(token:Long,context:Long,command:ByteArray,upload:ByteArray?):ByteArray
        @JvmStatic private external fun nativePresent(token:Long,context:Long,surface:Long,serial:Long):Int
        @JvmStatic private external fun nativeClose(token:Long)
        @JvmStatic external fun nativeSurface(surface:Surface?,generation:Long)
    }
}
