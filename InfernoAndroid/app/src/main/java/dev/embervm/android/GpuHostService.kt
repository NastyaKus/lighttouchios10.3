// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.app.Application
import android.app.Service
import android.content.Intent
import android.os.*
import android.os.Process
import android.view.Surface
import java.io.File
import org.json.JSONObject

internal object GpuHostNative {
    init { System.loadLibrary("ember_guest_gpu_probe") }
    @JvmStatic external fun nativeRun(image:String,log:String,timeout:Int):Array<String>
    @JvmStatic external fun nativeCancel()
    @JvmStatic external fun nativeSetSurface(surface:Surface?,generation:Long)
    @JvmStatic external fun nativePresentationMode(required:Boolean)
    @JvmStatic external fun nativePresentationStats():LongArray
}

/** No Activity/HWUI here: file-host currently owns its entire EGL display. */
class GpuHostService:Service() {
    private val main=Handler(Looper.getMainLooper())
    private var worker:Thread?=null
    private var cancelled=false
    private var activeRun=""
    private var owner:IBinder?=null
    private val ownerDeath=IBinder.DeathRecipient { main.post { cancel() } }
    private val endpoint=Messenger(Handler(Looper.getMainLooper()) { message ->
        when(message.what) {
            START -> start(message.data.getString("run") ?: "",message.data.getBoolean("present"),message.replyTo?.binder)
            CANCEL -> cancel()
            SURFACE -> {
                @Suppress("DEPRECATION") val surface=message.data.getParcelable<Surface>("surface")
                try { GpuHostNative.nativeSetSurface(surface,message.data.getLong("generation")) } finally { surface?.release() }
            }
            STATS -> runCatching {
                message.replyTo?.send(Message.obtain(null,STATS).apply { data=Bundle().apply {
                    putString("run",activeRun);putLongArray("stats",GpuHostNative.nativePresentationStats())
                } })
            }
        };true
    })
    override fun onBind(intent:Intent?)=endpoint.binder
    private fun start(run:String,present:Boolean,lease:IBinder?) {
        if(worker!=null || cancelled)return
        check(Application.getProcessName()==packageName+":gpu")
        require(GpuLabKit.runName.matches(run))
        // The preview can remain bound after the guest dies. Its UI binding
        // must not keep an orphaned executor alive or retain stale frame stats.
        if(lease==null){cancel();return}
        owner=lease
        try { lease.linkToDeath(ownerDeath,0) } catch(_:RemoteException) { cancel();return }
        activeRun=run
        GpuHostNative.nativePresentationMode(present)
        worker=Thread({
            val report=File(Prefs.root(this),"gpu-lab/runs/$run")
            try {
                check(Looper.myLooper()!=Looper.getMainLooper())
                val work=File(filesDir,"gpu-lab-scratch/$run")
                require(work.isDirectory && File(work,".ember-gpu-lab").isFile && report.isDirectory)
                val values=GpuHostNative.nativeRun(File(work,"InfernoData/xfer").path,File(report,"host.log").path,300_000)
                val host=GpuLabEvidence.decodeNative(values).copy(presentation=GpuPresentationStats.decode(GpuHostNative.nativePresentationStats()))
                GpuLabReports.atomic(File(report,"host-result.json"),GpuLabReports.hostJson(host).put("pid",Process.myPid()))
            } catch(e:Throwable) {
                runCatching { GpuLabReports.atomic(File(report,"host-result.json"),JSONObject().put("success",false)
                    .put("error","${e.javaClass.simpleName}: ${e.message}").put("pid",Process.myPid())) }
            }
        },"ember.gpu-host").apply { start() }
    }
    private fun cancel() {
        cancelled=true
        owner?.let { runCatching { it.unlinkToDeath(ownerDeath,0) } };owner=null
        if(worker!=null)runCatching { GpuHostNative.nativeCancel() }
        // A UI preview may remain bound. End this one-shot EGL process anyway;
        // Android reconnects the preview to a fresh service for the next run.
        Process.killProcess(Process.myPid())
    }
    override fun onDestroy() { cancel();super.onDestroy();Process.killProcess(Process.myPid()) }
    companion object { const val START=1;const val CANCEL=2;const val SURFACE=3;const val STATS=4 }
}
