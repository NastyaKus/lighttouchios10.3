// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.app.*
import android.content.*
import android.content.pm.ServiceInfo
import android.os.*
import android.os.Process
import android.system.Os
import dev.embervm.android.restore.NativeUnix
import dev.embervm.android.restore.RestoreRamdisk
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import org.json.JSONObject

/** Owns only a fresh disposable VM. QEMU is never restarted in this process. */
class GpuLabService:Service() {
    private val main=Handler(Looper.getMainLooper())
    private val cancelled=AtomicBoolean(false)
    private val hostDied=AtomicBoolean(false)
    private var worker:Thread?=null
    private var lease:VmLease?=null
    private var wake:PowerManager.WakeLock?=null
    @Volatile private var engineExited=false
    @Volatile private var engineStatus=-1
    @Volatile private var host:Messenger?=null
    private var connection:ServiceConnection?=null
    private var ending=false
    override fun onBind(intent:Intent?):IBinder?=null
    override fun onStartCommand(intent:Intent?,flags:Int,startId:Int):Int {
        if(intent?.action==STOP) {
            cancelled.set(true);worker?.interrupt()
            // Covers a blocked filesystem or vendor call; only disposable disks are attached.
            if(worker==null)finishProcess() else main.postDelayed({ finishProcess() },20_000)
            return START_NOT_STICKY
        }
        if(intent?.action!=START){stopSelfResult(startId);return START_NOT_STICKY}
        foreground("Подготовка GPU-теста")
        if(worker!=null || ending)return START_NOT_STICKY
        val broker=intent.getBooleanExtra(EXTRA_BROKER,false)
        val present=broker || intent.getBooleanExtra(EXTRA_PRESENT,false)
        worker=Thread({ runProbe(present,broker) },"ember.gpu-lab").apply { start() }
        return START_NOT_STICKY
    }
    private fun active() {
        if(cancelled.get() || Thread.currentThread().isInterrupted)throw InterruptedException("GPU-тест отменён")
    }
    private fun runProbe(present:Boolean,broker:Boolean) {
        val id="run-${System.currentTimeMillis()}-${UUID.randomUUID().toString().replace("-","").take(12)}"
        val scratch=File(filesDir,"gpu-lab-scratch").apply { mkdirs() };val work=File(scratch,id)
        var report:File?=null;var kit:GpuLabKit.Kit?=null;var listener:NativeUnix.Listener?=null
        var started=false;var passed=false;var failure:String?=null;var checks=0;var resultHost:GpuLabHostResult?=null
        var fingerprintAfter:String?=null;var rootBytes=0L
        val startedAt=SystemClock.elapsedRealtime()
        fun state(phase:String,text:String) { GpuLabReports.write(this,id,phase,text);notify(text) }
        try {
            check(Application.getProcessName()==packageName+":gpulab")
            report=GpuLabReports.run(this,id).apply { check(mkdirs()) }
            lease=VmLease.acquire(filesDir)
            GpuLabReports.prune(this)
            val root=Prefs.root(this)
            require(!File(root,"restore-prep.lock").exists() && !File(root,"restore-activation.plist").exists()) { "Сначала закончите подготовку/активацию набора" }
            require(BuildConfig.ENGINE_BUNDLED) { "В APK нет движка" }
            require(scratch.usableSpace>=GpuLabKit.MIN_SPACE) { "Для теста нужно 512 МиБ свободного места" }
            state("PREPARING","Подготовка временного набора")
            // Only marked disposable directories, under the exclusive machine lease.
            scratch.listFiles()?.filter { GpuLabKit.runName.matches(it.name) }?.forEach {
                runCatching { GpuLabKit.cleanup(scratch,it) }
            }
            wake=getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"ember:gpu-lab").apply { acquire(15*60*1000L) }
            val source=Prefs.config(this)
            val probe=assets.open(if(broker)"gpu/broker-launcher-ios" else if(present)"gpu/eagl-present-probe-ios" else "gpu/eagl-api-probe-ios").use { it.readBytes() }
            // Both existing NFC slots are executable. Broker mode uses a static
            // client in slot two, with no diagnostic raw-device entitlements.
            val library=assets.open(if(broker)"gpu/broker-client-ios" else "gpu/libEmberGLES.dylib").use { it.readBytes() }
            val prepared=GpuLabKit.prepare(source,work,{ stock,out -> RestoreRamdisk.buildGpuProbe(stock,out,probe,library) }) { active();state("PREPARING",it) }
            kit=prepared
            active();state("WAIT_GPU","Запуск отдельного EGL/GPU-процесса")
            bindHost(id,present)
            val channelUntil=SystemClock.elapsedRealtime()+20_000
            while(!GpuLabKit.channelReady(prepared.config.transferImage)) {
                active();require(!hostDied.get()) { "GPU-процесс завершился при запуске" }
                GpuLabReports.read(File(report,"host-result.json"))?.let { error("GPU-хост: ${it.optString("error")}") }
                require(SystemClock.elapsedRealtime()<channelUntil) { "GPU-хост не создал канал за 20 секунд" };Thread.sleep(100)
            }
            Engine.configureRenderer(RendererBackend.CPU)
            Engine.load()?.let { error("Движок: $it") }
            Engine.exitListener={ status -> engineStatus=status;engineExited=true }
            // A short path also works in secondary Android user profiles.
            val sockets=File(cacheDir,"gpu-lab-sock").apply { mkdirs() }
            File(sockets,VmConfig.USB_SOCKET).delete()
            listener=NativeUnix.Listener(File(sockets,VmConfig.USB_SOCKET)) // Deliberately never accept/restore.
            val args=GpuLabKit.arguments(prepared,prepareQemuData(this),Prefs.load(this).splitWx)
            prepared.config.guestConsoleLog.writeText("")
            state("BOOTING","Загрузка тестовой iOS: ожидание GLES/EAGL")
            active();check(Engine.start(args,prepared.config.engineLog.path,sockets.path)) { "Engine.start failed" };started=true
            val until=SystemClock.elapsedRealtime()+300_000;var hostFinishedAt=0L;var presenting=false
            while(true) {
                active();require(!engineExited) { "QEMU завершился раньше теста: $engineStatus" }
                val console=tail(prepared.config.guestConsoleLog,4000,2*1024*1024)
                if(present && !presenting && console.contains("*** EGPU-PRESENT: start")) {
                    presenting=true;state("PRESENTING","Гость рисует кадры для Android Surface")
                }
                require(!console.contains("panic(cpu")) { "Паника гостя; сохраните диагностику" }
                require(!console.contains("*** EGPU-GLES2: FAIL") && !console.contains("*** EGPU-EAGL: FAIL") && !console.contains("*** EGPU-PRESENT: FAIL")) { "Гостевой GLES/EAGL/present тест завершился с ошибкой" }
                require(!console.contains("*** EGPU-BROKER: FAIL") && !console.contains("*** EGPU-BROKER-CLIENT: FAIL")) { "Гостевой GPU-брокер завершился с ошибкой" }
                rootBytes=Os.stat(File(prepared.config.dataDirectory,"root").path).st_blocks*512
                require(rootBytes<=(16L shl 20)) { "Неожиданная запись в тестовый root" }
                val raw=GpuLabReports.read(File(report,"host-result.json"))
                if(raw!=null) {
                    val h=GpuLabReports.host(raw);require(h.success) { "GPU-хост: ${h.error}" }
                    if(hostFinishedAt==0L)hostFinishedAt=SystemClock.elapsedRealtime()
                    if(console.contains("*** EGPU-GLES2: PASS checks=") && (!broker || GpuBrokerEvidence.finished(console))) {
                        state("VERIFYING","Проверка пикселей, шейдеров и счётчиков")
                        checks=GpuLabEvidence.verify(console,h);resultHost=h
                        if(broker)GpuBrokerEvidence.verify(console,h.commands)
                        if(present) {
                            state("VERIFYING","Проверка кадра в Android Surface через PixelCopy")
                            val stats=h.presentation ?: error("Нет статистики present от GPU-хоста")
                            val displayUntil=SystemClock.elapsedRealtime()+10_000;var error:Throwable?=null
                            while(true) {
                                active()
                                val capture=GpuLabReports.read(File(report,"display-result.json"))
                                if(capture!=null) {
                                    val verdict=runCatching { GpuPresentationEvidence.verify(console,stats,id,GpuLabReports.capture(capture)) }
                                    if(verdict.isSuccess)break
                                    error=verdict.exceptionOrNull()
                                }
                                require(SystemClock.elapsedRealtime()<displayUntil) { "PixelCopy не подтвердил Surface: ${error?.message ?: "кадр не получен; держите экран стенда открытым"}" }
                                Thread.sleep(100)
                            }
                        }
                        break
                    }
                    require(SystemClock.elapsedRealtime()-hostFinishedAt<10_000) { "GPU-хост закончил, но гость не подтвердил PASS" }
                } else require(!hostDied.get()) { "GPU-процесс упал; сохраните диагностику" }
                require(SystemClock.elapsedRealtime()<until) { "Нет результата GPU-теста за 5 минут" }
                Thread.sleep(250)
            }
            state("STOPPING","Остановка тестовой VM")
            shutdown()
            require(engineExited && engineStatus==0) { "Нет подтверждения остановки QEMU: $engineStatus" }
            fingerprintAfter=GpuLabKit.fingerprint(File(prepared.config.dataDirectory,"root"))
            require(fingerprintAfter==prepared.rootFingerprint) { "Контрольные области тестового root изменились" }
            passed=true
        } catch(e:Throwable) {
            failure="${e.javaClass.simpleName}: ${e.message}"
            report?.let { runCatching { File(it,"run.log").appendText(android.util.Log.getStackTraceString(e)+"\n") } }
        } finally {
            Thread.interrupted() // Allow bounded shutdown after a cancellation interrupt.
            if(started && !engineExited)runCatching { shutdown() }
            runCatching { listener?.close() }
            runCatching { host?.send(Message.obtain(null,GpuHostService.CANCEL)) }
            connection?.let { runCatching { unbindService(it) } };connection=null
            val output=report
            if(output!=null)runCatching {
                kit?.let {
                    for((src,name) in listOf(it.config.guestConsoleLog to "guest-console.log",it.config.engineLog to "engine.log"))
                        if(src.isFile)File(output,name).writeText(tail(src,10000,2*1024*1024))
                }
                val result=JSONObject().put("passed",passed).put("checks",checks).put("run",id)
                    .put("elapsedMs",SystemClock.elapsedRealtime()-startedAt).put("error",failure ?: "")
                    .put("full_guest_gpu_acceleration",false).put("existing_guest_disks_used",false)
                    .put("start_restore_sent",false).put("usb_accepted",false)
                    .put("qemuExited",engineExited).put("qemuStatus",engineStatus)
                    .put("scratchRootPhysicalBytes",rootBytes).put("rootFingerprintBefore",kit?.rootFingerprint ?: "")
                    .put("rootFingerprintAfter",fingerprintAfter ?: "")
                    .put("physical_adreno_execution_verified",passed && resultHost?.renderer?.contains("Adreno",true)==true)
                    .put("presentationRequested",present).put("androidSurfacePixelsVerified",present && passed)
                    .put("brokerRequested",broker).put("brokerIsolationVerified",broker && passed)
                    .put("appStoreSandboxVerified",false)
                resultHost?.let { result.put("host",GpuLabReports.hostJson(it)) }
                GpuLabReports.atomic(File(output,"result.json"),result)
                state(if(passed)"PASSED" else if(cancelled.get())"CANCELLED" else "FAILED",
                    if(passed)if(broker)"Клиент mobile без доступа к raw-диску: IPC, 60 кадров и пиксели Surface проверены. Графика приложений ещё не подключена."
                        else if(present)"GLES/EAGL, 60 кадров и пиксели Android Surface проверены. Графика приложений ещё не подключена."
                        else "Пройдено $checks проверок GLES/EAGL. Графика обычных приложений ещё не подключена."
                    else failure ?: "Тест прерван")
            }
            // Never unlink files while QEMU could still be accessing them.
            if(!started || engineExited)runCatching { GpuLabKit.cleanup(scratch,work) }
            main.post { finishProcess() }
        }
    }
    private fun bindHost(run:String,present:Boolean) {
        val ready=CountDownLatch(1)
        val conn=object:ServiceConnection {
            override fun onServiceConnected(name:ComponentName,binder:IBinder) { host=Messenger(binder);ready.countDown() }
            override fun onServiceDisconnected(name:ComponentName) { hostDied.set(true);ready.countDown() }
            override fun onNullBinding(name:ComponentName) { hostDied.set(true);ready.countDown() }
            override fun onBindingDied(name:ComponentName) { hostDied.set(true);ready.countDown() }
        }
        check(bindService(Intent(this,GpuHostService::class.java),conn,BIND_AUTO_CREATE or BIND_IMPORTANT)) { "Не удалось запустить GPU-процесс" }
        connection=conn
        require(ready.await(10,TimeUnit.SECONDS) && !hostDied.get()) { "GPU-процесс не подключился" }
        host!!.send(Message.obtain(null,GpuHostService.START).apply {
            replyTo=Messenger(main)
            data=Bundle().apply { putString("run",run);putBoolean("present",present) }
        })
    }
    private fun shutdown() {
        val until=SystemClock.elapsedRealtime()+15_000
        while(!engineExited && SystemClock.elapsedRealtime()<until) {
            Engine.requestShutdown();Thread.sleep(100)
        }
    }
    private fun finishProcess() {
        if(ending)return
        ending=true
        connection?.let { runCatching { host?.send(Message.obtain(null,GpuHostService.CANCEL));unbindService(it) } }
        wake?.let { if(it.isHeld)it.release() }
        stopForeground(STOP_FOREGROUND_REMOVE);stopSelf()
        // Keep the file lock until the kernel has actually destroyed QEMU.
        main.postDelayed({ Process.killProcess(Process.myPid()) },500)
    }
    private fun foreground(text:String) {
        if(Build.VERSION.SDK_INT>=34)startForeground(31,notification(text),ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        else startForeground(31,notification(text))
    }
    private fun notify(text:String)=getSystemService(NotificationManager::class.java).notify(31,notification(text))
    private fun notification(text:String):Notification {
        getSystemService(NotificationManager::class.java).createNotificationChannel(NotificationChannel("gpu-lab","GPU-тест",NotificationManager.IMPORTANCE_LOW))
        val open=PendingIntent.getActivity(this,31,Intent(this,GpuLabActivity::class.java),PendingIntent.FLAG_IMMUTABLE)
        val stop=PendingIntent.getService(this,32,Intent(this,GpuLabService::class.java).setAction(STOP),PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this,"gpu-lab").setSmallIcon(R.drawable.ic_stat).setContentTitle("Ember VM · GPU-тест")
            .setContentText(text).setContentIntent(open).setOngoing(true)
            .addAction(Notification.Action.Builder(null,"Остановить тест",stop).build()).build()
    }
    companion object { const val START="dev.embervm.android.GPU_LAB";const val STOP="dev.embervm.android.GPU_LAB_STOP";const val EXTRA_PRESENT="present";const val EXTRA_BROKER="broker" }
}
