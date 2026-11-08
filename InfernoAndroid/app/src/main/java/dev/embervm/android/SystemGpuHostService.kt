// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.app.Application
import android.app.Service
import android.content.Intent
import android.os.*
import android.os.Process
import android.view.Surface
import java.util.concurrent.TimeUnit

/** Bound to a VM process for one boot. Does not use the EGP1 lab disk, probe
 * program, or five-minute lab timeout. Registration alone does not activate a
 * guest GPU profile; the installer/guest frontend are separate prerequisites. */
class SystemGpuHostService:Service() {
    private val main=Handler(Looper.getMainLooper())
    private var gpu:SystemGpuEndpoint?=null
    private var owner:IBinder?=null
    private var run:ByteArray?=null
    private var stopping=false
    private var reply:Messenger?=null
    private val watchdog=object:Runnable {
        override fun run() {
            if(stopping)return
            val endpoint=gpu ?: return
            val failure=endpoint.failure
            if(failure!=null || endpoint.awaitClosed(0,TimeUnit.MILLISECONDS)) {
                val message=Message.obtain().apply { replyTo=reply }
                respond(message,FAILED,failure?.let { "${it.javaClass.simpleName}: ${it.message}" } ?: "GPU channel closed")
                stop()
            } else main.postDelayed(this,250)
        }
    }
    private val ownerDeath=IBinder.DeathRecipient { main.post { stop() } }
    private val messenger=Messenger(Handler(Looper.getMainLooper()) { message ->
        try {
            when(message.what) {
                START -> start(message)
                STOP -> if(sameRun(message))stop()
                SURFACE -> {
                    @Suppress("DEPRECATION") val surface=message.data.getParcelable<Surface>("surface")
                    try { if(sameRun(message) && !stopping) SystemGpuNativeBackend.nativeSurface(surface,message.data.getLong("generation")) }
                    finally { surface?.release() }
                }
                STATUS -> if(sameRun(message))respond(message,STATE,gpu?.failure?.let { "${it.javaClass.simpleName}: ${it.message}" })
            }
        } catch(e:Exception) { respond(message,FAILED,"${e.javaClass.simpleName}: ${e.message}");stop() }
        true
    })
    override fun onCreate() {
        super.onCreate()
        check(Application.getProcessName()==packageName+":system_gpu") { "System GPU must own its isolated process" }
    }
    override fun onBind(intent:Intent?)=messenger.binder
    private fun sameRun(message:Message)=run?.contentEquals(message.data.getByteArray("run"))==true
    private fun start(message:Message) {
        check(!stopping && gpu==null) { "A system GPU run already exists" }
        val identity=message.data.getByteArray("run") ?: error("Missing run identity")
        val secret=message.data.getByteArray("secret") ?: error("Missing per-boot capability")
        try {
            require(identity.size==16 && secret.size==32)
            val lease=message.replyTo?.binder ?: error("Missing VM lifetime binder")
            lease.linkToDeath(ownerDeath,0);owner=lease;run=identity.copyOf();reply=message.replyTo
            gpu=SystemGpuEndpoint(secret,identity,{ SystemGpuNativeBackend() })
            respond(message,READY)
            main.postDelayed(watchdog,250)
        } finally { secret.fill(0) }
    }
    private fun respond(message:Message,what:Int,error:String?=null) {
        runCatching { message.replyTo?.send(Message.obtain(null,what).apply { data=Bundle().apply {
            putByteArray("run",run);putInt("port",gpu?.port ?: 0)
            putBoolean("authenticated",gpu?.authenticated==true);putBoolean("stopping",stopping)
            if(error!=null)putString("error",error.take(512))
        } }) }
    }
    private fun stop() {
        if(stopping)return
        stopping=true;gpu?.close()
        main.removeCallbacks(watchdog);reply=null
        owner?.let { runCatching { it.unlinkToDeath(ownerDeath,0) } };owner=null
        // A vendor GL call may ignore cancellation. Close the socket first;
        // contain a stuck driver in this process, never in the VM or launcher.
        main.postDelayed({ Process.killProcess(Process.myPid()) },2000)
        stopSelf()
    }
    override fun onDestroy() { stop();super.onDestroy() }
    companion object {
        const val START=1;const val STOP=2;const val SURFACE=3;const val STATUS=4
        const val READY=10;const val STATE=11;const val FAILED=12
    }
}
