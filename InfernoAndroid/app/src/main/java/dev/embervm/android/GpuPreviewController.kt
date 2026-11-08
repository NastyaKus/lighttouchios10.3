// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.content.*
import android.graphics.Bitmap
import android.os.*
import android.view.PixelCopy
import android.view.SurfaceHolder
import android.view.SurfaceView
import kotlinx.coroutines.flow.MutableStateFlow
import org.json.JSONArray
import org.json.JSONObject
import java.io.Closeable
import java.io.File

/** UI owns the consumer Surface; only :gpu ever calls EGL on its producer. */
class GpuPreviewController(private val context:Context):SurfaceHolder.Callback,Closeable {
    val message=MutableStateFlow("Ожидание GPU-кадра")
    var currentRun=""
        set(value) {
            if(value!=field){field=value;captured="";message.value="Ожидание кадра текущего запуска"}
        }
    var captureEnabled=false
    private val main=Handler(Looper.getMainLooper())
    private var holder:SurfaceHolder?=null
    private var generation=SystemClock.elapsedRealtimeNanos()
    private var remote:Messenger?=null
    private var bound=false
    private var closed=false
    private var copying=false
    private var captured=""
    private val reply=Messenger(Handler(Looper.getMainLooper()) { msg ->
        if(!closed && msg.what==GpuHostService.STATS) {
            val run=msg.data.getString("run").orEmpty()
            msg.data.getLongArray("stats")?.let { values ->
                runCatching { maybeCapture(run,GpuPresentationStats.decode(values)) }
                    .onFailure { message.value="Проверка кадра: ${it.message}" }
            }
        };true
    })
    private val connection=object:ServiceConnection {
        override fun onServiceConnected(name:ComponentName,binder:IBinder) { remote=Messenger(binder);sendSurface() }
        override fun onServiceDisconnected(name:ComponentName) { remote=null }
        override fun onBindingDied(name:ComponentName) { remote=null }
    }
    fun bind() {
        if(bound || closed)return
        bound=context.bindService(Intent(context,GpuHostService::class.java),connection,Context.BIND_AUTO_CREATE)
        main.post(poll)
    }
    private val poll=object:Runnable {
        override fun run() {
            if(closed)return
            if(captureEnabled)runCatching { remote?.send(Message.obtain(null,GpuHostService.STATS).apply { replyTo=reply }) }
            main.postDelayed(this,250)
        }
    }
    fun attach(view:SurfaceView) { view.holder.addCallback(this);view.keepScreenOn=true }
    override fun surfaceCreated(surface:SurfaceHolder) { holder=surface;generation++;captured="";sendSurface() }
    override fun surfaceChanged(surface:SurfaceHolder,format:Int,width:Int,height:Int) { holder=surface;generation++;captured="";sendSurface() }
    override fun surfaceDestroyed(surface:SurfaceHolder) { holder=null;generation++;sendSurface() }
    private fun sendSurface() {
        val surface=holder?.surface?.takeIf { it.isValid }
        runCatching { remote?.send(Message.obtain(null,GpuHostService.SURFACE).apply { data=Bundle().apply {
            putParcelable("surface",surface);putLong("generation",generation)
        } }) }
    }
    private fun maybeCapture(run:String,stats:GpuPresentationStats) {
        if(!captureEnabled || run!=currentRun || !GpuLabKit.runName.matches(run) || copying || stats.windowFrames==0L ||
            stats.generation!=generation || captured=="$run:$generation")return
        val surface=holder?.surface?.takeIf { it.isValid } ?: return
        val bitmap=Bitmap.createBitmap(64,64,Bitmap.Config.ARGB_8888)
        copying=true
        fun finish(code:Int) {
            try {
                if(closed || run!=currentRun || stats.generation!=generation)return
                val observed=if(code==PixelCopy.SUCCESS)listOf(8 to 8,56 to 8,8 to 56,56 to 56)
                    .map { (x,y) -> GpuPresentationEvidence.rgba(bitmap.getPixel(x,y)) } else emptyList()
                val matched=code==PixelCopy.SUCCESS && GpuPresentationEvidence.pixelsMatch(stats.bitmapCorners,observed)
                val result=JSONObject().put("run",run).put("generation",stats.generation).put("serial",stats.serial)
                    .put("pixelCopyResult",code).put("expected",JSONArray(stats.bitmapCorners)).put("observed",JSONArray(observed))
                    .put("matched",matched).put("time",System.currentTimeMillis())
                GpuLabReports.atomic(File(GpuLabReports.run(context,run),"display-result.json"),result)
                if(matched){captured="$run:${stats.generation}";message.value="Кадр Android Surface проверен: пиксели совпали"}
                else message.value="Ожидание совпадения пикселей Surface (PixelCopy=$code)"
            } catch(e:Exception) { message.value="Не удалось сохранить проверку Surface: ${e.message}" }
            finally { copying=false;bitmap.recycle() }
        }
        try { PixelCopy.request(surface,bitmap,::finish,main) } catch(_:IllegalArgumentException) { finish(-1) }
    }
    override fun close() {
        if(closed)return
        holder=null;generation++;sendSurface();closed=true;main.removeCallbacks(poll)
        if(bound)context.unbindService(connection)
        bound=false;remote=null
    }
}
