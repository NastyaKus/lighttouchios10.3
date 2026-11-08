// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.*
import android.view.Surface
import java.io.Closeable

/** VM-side lifetime owner. The UI may forward a Surface, but its binding must
 * never keep a GPU session alive after the VM process dies. Must run on main.
 * No normal VM path constructs this until a valid guest frontend is installed. */
internal class SystemGpuHostConnection(
    context:Context,run:ByteArray,secret:ByteArray,
    private val onReady:(Int)->Unit,private val onFailure:(String)->Unit
): ServiceConnection,Closeable {
    private val app=context.applicationContext
    private val identity=run.copyOf().also { require(it.size==16) }
    private val capability=secret.copyOf().also { require(it.size==32) }
    private var remote:Messenger?=null
    private var bound=false
    private var closed=false
    private var ready=false
    private val main=Handler(Looper.getMainLooper())
    private val callback=Messenger(Handler(Looper.getMainLooper()) { message ->
        if(!closed && identity.contentEquals(message.data.getByteArray("run"))) {
            when(message.what) {
                SystemGpuHostService.READY -> {
                    val port=message.data.getInt("port")
                    if(ready || port !in 1..65535)failed("Invalid system GPU readiness response")
                    else {ready=true;onReady(port);main.postDelayed(poll,1000)}
                }
                SystemGpuHostService.STATE -> {
                    val error=message.data.getString("error")
                    if(error!=null)failed(error) else if(message.data.getBoolean("stopping"))failed("System GPU service stopped")
                    else main.postDelayed(poll,1000)
                }
                SystemGpuHostService.FAILED -> failed(message.data.getString("error") ?: "System GPU service failed")
            }
        };true
    })
    private val poll=object:Runnable {
        override fun run(){if(!closed && ready)send(SystemGpuHostService.STATUS)}
    }
    init {
        check(Looper.myLooper()==Looper.getMainLooper())
        bound=app.bindService(Intent(app,SystemGpuHostService::class.java),this,Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT)
        if(!bound){capability.fill(0);closed=true;onFailure("Cannot bind system GPU service")}
    }
    override fun onServiceConnected(name:ComponentName?,binder:IBinder?) {
        if(closed || binder==null)return
        remote=Messenger(binder)
        val data=Bundle().apply { putByteArray("secret",capability.copyOf()) }
        try { send(SystemGpuHostService.START,data) } finally { capability.fill(0);data.getByteArray("secret")?.fill(0) }
    }
    private fun send(what:Int,data:Bundle=Bundle()) {
        if(closed)return
        data.putByteArray("run",identity)
        try { remote?.send(Message.obtain(null,what).apply { this.data=data;replyTo=this@SystemGpuHostConnection.callback }) }
        catch(e:RemoteException) { failed("System GPU connection lost: ${e.javaClass.simpleName}") }
    }
    fun setSurface(surface:Surface?,generation:Long) {
        check(Looper.myLooper()==Looper.getMainLooper())
        if(!closed)send(SystemGpuHostService.SURFACE,Bundle().apply { putParcelable("surface",surface);putLong("generation",generation) })
    }
    private fun failed(message:String) { if(!closed){close();onFailure(message.take(512))} }
    override fun onServiceDisconnected(name:ComponentName?)=failed("System GPU process disconnected")
    override fun onBindingDied(name:ComponentName?)=failed("System GPU binding died")
    override fun onNullBinding(name:ComponentName?)=failed("System GPU binding unavailable")
    override fun close() {
        check(Looper.myLooper()==Looper.getMainLooper())
        if(closed)return
        closed=true;ready=false;capability.fill(0)
        runCatching { remote?.send(Message.obtain(null,SystemGpuHostService.STOP).apply {
            data=Bundle().apply { putByteArray("run",identity) };replyTo=this@SystemGpuHostConnection.callback
        }) }
        main.removeCallbacks(poll);remote=null
        if(bound){app.unbindService(this);bound=false}
    }
}
