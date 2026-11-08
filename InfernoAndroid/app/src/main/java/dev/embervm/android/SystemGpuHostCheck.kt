// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.content.Context
import android.graphics.Bitmap
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.PixelCopy
import android.view.SurfaceHolder
import android.view.SurfaceView
import kotlinx.coroutines.flow.MutableStateFlow
import java.io.Closeable
import java.io.File
import java.security.SecureRandom
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** GPU lab check of the persistent system GPU host on this phone's driver:
 * :system_gpu executes the guest-shaped EGP2 scenario sent from this process
 * and presents it into [attach]ed SurfaceView, which PixelCopy then reads.
 * No VM, iOS disk or guest program is involved. Main thread only. */
class SystemGpuHostCheck(context:Context):SurfaceHolder.Callback,Closeable {
    data class State(val busy:Boolean=false,val message:String="Не запускалась",val result:String="")
    val state=MutableStateFlow(State(result=lastResult(context)))
    private val app=context.applicationContext
    private val main=Handler(Looper.getMainLooper())
    private var holder:SurfaceHolder?=null
    private var generation=SystemClock.elapsedRealtimeNanos()
    private var connection:SystemGpuHostConnection?=null
    private var closed=false
    private var workerRunning=false
    /** Set once the client has captured the window and begins its teardown. */
    @Volatile private var tearingDown=false
    private var hostFailure:String?=null
    @Volatile private var pixelCopy:Int?=null
    @Volatile private var pixels:List<List<Int>>?=null

    fun attach(view:SurfaceView) { view.holder.addCallback(this);view.keepScreenOn=true }
    override fun surfaceCreated(surface:SurfaceHolder) { holder=surface;generation++;forward() }
    override fun surfaceChanged(surface:SurfaceHolder,format:Int,width:Int,height:Int) { holder=surface;generation++;forward() }
    override fun surfaceDestroyed(surface:SurfaceHolder) { holder=null;generation++;forward() }
    private fun forward() { connection?.setSurface(holder?.surface?.takeIf { it.isValid },generation) }

    fun start() {
        check(Looper.myLooper()==Looper.getMainLooper())
        if(closed || state.value.busy)return
        if(holder?.surface?.isValid!=true){state.value=state.value.copy(message="Экран проверки ещё не готов");return}
        val random=SecureRandom()
        val run=ByteArray(16).also(random::nextBytes)
        val secret=ByteArray(32).also(random::nextBytes)
        val clientSecret=secret.copyOf()
        hostFailure=null;pixelCopy=null;pixels=null;workerRunning=false;tearingDown=false
        state.value=State(busy=true,message="Запуск GPU-процесса…",result=state.value.result)
        val started=SystemClock.elapsedRealtime()
        connection=SystemGpuHostConnection(app,run,secret,onReady={ port ->
            forward()
            state.value=state.value.copy(message="Хост готов, выполняю команды и ${SystemGpuSelfTest.FRAMES} кадров…")
            workerRunning=true
            Thread({
                val report=runCatching { SystemGpuSelfTest.run(port,clientSecret,run,SystemGpuSelfTest.FRAMES) { capture() } }
                    .getOrElse { SystemGpuSelfTest.parse("passed=0\nstage=${it.javaClass.simpleName}\n") }
                clientSecret.fill(0)
                main.post { finish(report,SystemClock.elapsedRealtime()-started) }
            },"ember.system-gpu-selftest").start()
        },onFailure={ message ->
            // The host reports the channel closing once our client is done; only
            // a failure while the client still runs belongs to the result.
            if(workerRunning){ if(!tearingDown)hostFailure=message }
            else if(state.value.busy){clientSecret.fill(0);finish(SystemGpuSelfTest.parse("passed=0\nstage=host-start\n"),
                SystemClock.elapsedRealtime()-started,message)}
        })
        secret.fill(0)
    }

    /** Called on the client thread after the last present, before teardown. */
    private fun capture():Boolean {
        val latch=CountDownLatch(1)
        main.post {
            val surface=holder?.surface?.takeIf { it.isValid }
            if(closed || surface==null){pixelCopy=-1;latch.countDown();return@post}
            val bitmap=Bitmap.createBitmap(64,64,Bitmap.Config.ARGB_8888)
            try {
                PixelCopy.request(surface,bitmap,{ code ->
                    pixelCopy=code
                    if(code==PixelCopy.SUCCESS)pixels=listOf(8 to 8,56 to 8,8 to 56,56 to 56).map { (x,y) ->
                        val c=bitmap.getPixel(x,y);listOf(c shr 16 and 255,c shr 8 and 255,c and 255,c ushr 24)
                    }
                    bitmap.recycle();latch.countDown()
                },main)
            } catch(_:IllegalArgumentException) { pixelCopy=-2;bitmap.recycle();latch.countDown() }
        }
        latch.await(5,TimeUnit.SECONDS)
        tearingDown=true
        return true // the verdict, not the client, judges the captured pixels
    }

    private fun finish(report:SystemGpuSelfTest.Report,elapsed:Long,startFailure:String?=null) {
        if(!state.value.busy)return
        workerRunning=false
        val failure=startFailure ?: hostFailure
        val problems=SystemGpuSelfTest.problems(report,SystemGpuSelfTest.FRAMES,pixels,failure)
        val json=SystemGpuSelfTest.json(report,SystemGpuSelfTest.FRAMES,pixelCopy,pixels,failure,problems)
            .put("elapsedMs",elapsed).put("device","${Build.MANUFACTURER} ${Build.MODEL}").put("android",Build.VERSION.SDK_INT)
            .put("app","${BuildConfig.APPLICATION_ID} ${BuildConfig.VERSION_NAME}")
        val text=json.toString(2)
        runCatching { GpuLabReports.atomic(file(app),json) }
        connection?.close();connection=null
        state.value=State(false,if(problems.isEmpty())"PASSED: системный GPU-хост работает на ${report.renderer}"
            else "FAILED: ${problems.first()}",text)
    }

    override fun close() {
        if(closed)return
        closed=true;connection?.close();connection=null;holder=null
    }

    companion object {
        fun file(context:Context)=File(GpuLabReports.directory(context),"system-host-selftest.json")
        private fun lastResult(context:Context)=runCatching { file(context).takeIf { it.isFile }?.readText() }.getOrNull().orEmpty()
    }
}
