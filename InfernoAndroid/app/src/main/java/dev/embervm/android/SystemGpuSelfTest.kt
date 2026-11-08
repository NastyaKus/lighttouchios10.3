// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import org.json.JSONArray
import org.json.JSONObject

/** Client half of the system GPU host self-test: the guest-shaped EGP2
 * scenario of gpu-bridge/system/selftest.c, run against :system_gpu over
 * loopback. A pass means this phone's driver runs the persistent host path
 * end to end. It does not involve iOS and is not iOS acceleration. */
object SystemGpuSelfTest {
    const val PANEL_BYTES=828*1792*4
    const val FRAMES=180
    /** The texel every pixel of the panel samples (RGBA). */
    val EXPECTED_RGBA=listOf(220,45,7,255)

    fun interface Hold { fun presented():Boolean }

    data class Report(val values:Map<String,String>) {
        val passed get()=values["passed"]=="1"
        val stage get()=values["stage"].orEmpty()
        val renderer get()=values["renderer"].orEmpty()
        fun int(key:String)=values[key]?.toLongOrNull() ?: -1L
        fun double(key:String)=values[key]?.toDoubleOrNull() ?: Double.NaN
    }

    fun parse(text:String)=Report(text.lineSequence().mapNotNull { line ->
        val at=line.indexOf('=')
        if(at<=0)null else line.substring(0,at) to line.substring(at+1)
    }.toMap())

    private val loaded by lazy { System.loadLibrary("ember_system_gpu_selftest");true }

    /** Blocking; never on the main thread. secret/run are not retained. */
    fun run(port:Int,secret:ByteArray,run:ByteArray,frames:Int=FRAMES,hold:Hold?=null):Report {
        require(secret.size==32 && run.size==16 && frames in 0..600)
        check(loaded)
        val capability=secret+run
        try { return parse(nativeRun(port,capability,frames,hold) ?: "passed=0\nstage=jni\n") }
        finally { capability.fill(0) }
    }

    /** Tolerates scaling/dither of the window path, not a different colour. */
    fun pixelMatches(observed:List<Int>,tolerance:Int=4)=observed.size==4 &&
        observed.zip(EXPECTED_RGBA).all { (o,e) -> kotlin.math.abs(o-e)<=tolerance }

    /** Everything a PASSED verdict requires; empty means passed. */
    fun problems(report:Report,frames:Int,pixels:List<List<Int>>?,hostFailure:String?):List<String> {
        val out=mutableListOf<String>()
        if(!report.passed)out+="Сценарий остановился на шаге «${report.stage}»"
        if(report.int("readback_bytes")!=PANEL_BYTES.toLong())out+="Чтение полного кадра 828×1792 не подтверждено"
        if(report.int("presents")!=frames.toLong())out+="Выполнено ${report.int("presents")} из $frames выводов"
        if(report.int("presented_to_window")<=0)out+="Ни один кадр не попал в Android Surface"
        if(report.renderer.isBlank())out+="Хост не сообщил GL_RENDERER"
        when {
            pixels==null -> out+="PixelCopy не выполнен"
            pixels.size!=4 || !pixels.all(::pixelMatches) -> out+="Пиксели Surface не совпали с ожидаемым цветом"
        }
        if(hostFailure!=null)out+="Хост: $hostFailure"
        return out
    }

    fun json(report:Report,frames:Int,pixelCopy:Int?,pixels:List<List<Int>>?,hostFailure:String?,problems:List<String>):JSONObject =
        JSONObject().put("kind","system-gpu-host-selftest").put("passed",problems.isEmpty())
            .put("problems",JSONArray(problems)).put("report",JSONObject(report.values))
            .put("frames",frames).put("pixelCopyResult",pixelCopy ?: JSONObject.NULL)
            .put("expectedRgba",JSONArray(EXPECTED_RGBA))
            .put("observedRgba",pixels?.let { JSONArray(it.map(::JSONArray)) } ?: JSONObject.NULL)
            .put("hostFailure",hostFailure ?: JSONObject.NULL)
            .put("iosGpuVerified",false).put("time",System.currentTimeMillis())

    @JvmStatic private external fun nativeRun(port:Int,capability:ByteArray,frames:Int,hold:Hold?):String?
}
