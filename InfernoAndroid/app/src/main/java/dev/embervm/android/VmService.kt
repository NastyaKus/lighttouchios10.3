package dev.embervm.android

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.BatteryManager
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.Process
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.File

/** What the :vm process is doing, for [VmActivity] (same process). */
object VmStatus {
    sealed interface State {
        data object Idle : State
        data object Booting : State
        data object Running : State
        data class Stopped(val status: Int) : State
        data class Failed(val reason: String) : State
    }
    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state
    internal fun set(s: State) { _state.value = s }
}

/**
 * Owns the machine. Runs in the :vm process with the activity that shows it:
 * QEMU can be initialised only once per process and calls exit() on a bad
 * argument, so it gets a process of its own, the launcher survives whatever
 * happens here, and every start is a fresh process.
 */
class VmService : Service() {
    private val main = Handler(Looper.getMainLooper())
    private var wakeLock: PowerManager.WakeLock? = null
    private var batteryReceiver: BroadcastReceiver? = null
    private var stopping = false
    private var finished = false
    private var restoreSession: dev.embervm.android.restore.RestoreSession? = null
    private var restoreWorker: Thread? = null
    private var graphics: GraphicsTelemetry? = null
    private var vmLease: VmLease? = null

    private fun claimMachine():Boolean {
        if(vmLease!=null)return true
        vmLease=VmLease.tryAcquire(filesDir)
        if(vmLease!=null)return true
        // Do not replace the working kit's logs when a diagnostic owns the VM.
        VmStatus.set(VmStatus.State.Failed("Другая VM или GPU-тест ещё работают"))
        finishProcess()
        return false
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        RestoreDiagnostics.lifecycle(this, "service action=${intent?.action}")
        when (intent?.action) {
            ACTION_START -> start(intent)
            ACTION_RESTORE -> startRestore(intent.getBooleanExtra(EXTRA_PATCH_ONLY,false))
            ACTION_STOP -> stop()
        }
        return START_NOT_STICKY
    }

    private fun start(intent: Intent) {
        foreground("Загрузка iOS…")
        if (Engine.state != Engine.STATE_IDLE) return
        if (!claimMachine()) return
        if (File(Prefs.root(this), "restore-prep.lock").exists() || File(Prefs.root(this), "restore-activation.plist").exists()) return fail("Подготовка или незавершённая активация набора")
        val args = intent.getStringArrayExtra(EXTRA_ARGS)?.toList() ?: return fail("нет аргументов запуска")
        val haptics = intent.getBooleanExtra(EXTRA_HAPTICS, true)
        val config = Prefs.config(this)

        val settings = Prefs.load(this).copy(renderer = RendererBackend.parse(intent.getStringExtra(EXTRA_RENDERER)))
        val selected = Prefs.graphicsSafety(this).select(settings.renderer)
        Engine.configureRenderer(selected)

        Engine.load()?.let { return fail("Движок не загрузился: $it") }
        runCatching { prepareQemuData(this) }.onFailure { return fail("Файлы движка: ${it.message}") }

        config.logDirectory.mkdirs()
        // The console log is appended to by the engine; start each run clean.
        runCatching { config.guestConsoleLog.writeText("") }
        Prefs.exitMarker(this).delete()

        Engine.exitListener = { status -> main.post { onExit(status) } }
        if (haptics) Engine.hapticListener = ::vibrate

        val socketDir = File(cacheDir, "sock").apply { mkdirs() }
        File(socketDir, VmConfig.USB_SOCKET).delete()
        if (!Engine.start(args, config.engineLog.path, socketDir.path)) return fail("Движок не запустился")
        // Diagnostics must not bring down a running VM when storage is full.
        graphics = runCatching {
            GraphicsTelemetry(this, settings, selected, selected != settings.renderer).also { it.start() }
        }.onFailure { Log.w("ember.graphics", "Cannot start telemetry", it) }.getOrNull()

        VmStatus.set(VmStatus.State.Booting)
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "inferno:vm").apply { acquire(24*60*60*1000L) }
        watchBattery()
        pollUntilRunning()
    }

    private fun pollUntilRunning() {
        main.postDelayed({
            when (Engine.state) {
                Engine.STATE_RUNNING -> {
                    VmStatus.set(VmStatus.State.Running)
                    notify("Виртуальная машина запущена")
                }
                Engine.STATE_STARTING -> pollUntilRunning()
            }
        }, 200)
    }

    /** Asks the machine to stop; if it does not within a while, the process goes. */
    private fun stop() {
        if (stopping) return
        stopping = true
        RestoreDiagnostics.lifecycle(this, "stop requested engineState=${Engine.state}")
        restoreSession?.close()
        restoreWorker?.interrupt()
        if (Engine.state == Engine.STATE_RUNNING) {
            notify("Выключение…")
            Engine.requestShutdown()
            main.postDelayed({ finishProcess() }, 15_000)
        } else {
            finishProcess()
        }
    }

    private fun onExit(status: Int) {
        if (restoreSession == null && status == 0) awaitRendererStop(15)
        RestoreDiagnostics.lifecycle(this, "engine exited status=$status")
        Log.i("inferno", "машина остановлена ($status)")
        runCatching { Prefs.exitMarker(this).writeText(status.toString()) }
        VmStatus.set(VmStatus.State.Stopped(status))
        restoreSession?.let {
            it.engineExitStatus = status
            it.engineExited = true
            return
        }
        finishProcess()
    }

    private fun awaitRendererStop(attempts: Int) {
        if (Engine.rendererStopped()) Prefs.graphicsSafety(this).cleanExit(0)
        else if (attempts > 0) main.postDelayed({ awaitRendererStop(attempts-1) }, 100)
        // If the driver is still stuck, finishProcess kills :vm and keeps the latch.
    }

    private fun startRestore(patchOnly:Boolean) {
        foreground("Подготовка восстановления…")
        if (restoreWorker?.isAlive == true || restoreSession != null || Engine.state != Engine.STATE_IDLE || stopping) {
            RestoreDiagnostics.lifecycle(this, "duplicate restore request ignored")
            return
        }
        if (!claimMachine()) return
        Engine.configureRenderer(RendererBackend.CPU)
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ember:restore").apply { acquire(24*60*60*1000L) }
        val session = dev.embervm.android.restore.RestoreSession(this) { text, fraction ->
            RestoreState.write(this, "RESTORING", text, fraction)
            notify("$text · ${(fraction * 100).toInt()} %")
        }
        restoreSession = session
        Engine.exitListener = { status -> main.post { onExit(status) } }
        restoreWorker = Thread({
            try {
                RestoreDiagnostics.lifecycle(this, "restore worker starting patchOnly=$patchOnly")
                val result = session.run(patchOnly)
                val state = RestoreState.read(this)
                val complete = result == dev.embervm.android.restore.RestoreSession.Result.COMPLETE
                RestoreState.write(this, if (complete) "COMPLETE" else "INCOMPLETE", state.message, state.fraction)
            } catch (e: Exception) {
                Log.e("ember.restore", "Restore failed", e)
                runCatching { File(Prefs.root(this), "restore-error.log").writeText(android.util.Log.getStackTraceString(e)) }
                RestoreDiagnostics.lifecycle(this, "restore failed ${e.javaClass.simpleName}: ${e.message}")
                RestoreState.write(this, if (e is InterruptedException) "CANCELLED" else "FAILED", e.message ?: e.javaClass.simpleName)
            } finally {
                session.close()
                main.post {
                    restoreSession = null
                    stop()
                    // stop() may already have been called by the user.
                    if (stopping) main.postDelayed({ finishProcess() }, 15000)
                }
            }
        }, "ember.restore").apply { start() }
    }

    private fun fail(reason: String) {
        Log.e("inferno", reason)
        runCatching {
            val config = Prefs.config(this)
            config.logDirectory.mkdirs()
            config.engineLog.writeText(reason + "\n")
        }
        VmStatus.set(VmStatus.State.Failed(reason))
        main.postDelayed({ finishProcess() }, 4000)
    }

    /** QEMU cannot be started twice in one process: the next start gets a new one. */
    private fun finishProcess() {
        if (finished) return
        finished = true
        graphics?.stop(); graphics = null
        RestoreDiagnostics.lifecycle(this, "controlled VM process shutdown; status=${RestoreState.read(this).phase}")
        wakeLock?.let { if (it.isHeld) it.release() }
        batteryReceiver?.let { runCatching { unregisterReceiver(it) } }
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
        // Keep ownership until the process with QEMU is actually going away.
        // The kernel releases vmLease on process death, with no overlap window.
        main.postDelayed({ Process.killProcess(Process.myPid()) }, 2000)
    }

    /** The phone's own battery is the one the guest shows. */
    private fun watchBattery() {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
                val plugged = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0
                val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
                val charging = status == BatteryManager.BATTERY_STATUS_CHARGING
                if (level >= 0 && scale > 0) Engine.battery(level * 100 / scale, plugged, charging)
            }
        }
        batteryReceiver = receiver
        registerReceiver(receiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    }

    @Suppress("DEPRECATION")
    private val vibrator: Vibrator by lazy {
        if (Build.VERSION.SDK_INT >= 31) getSystemService(VibratorManager::class.java).defaultVibrator
        else getSystemService(VIBRATOR_SERVICE) as Vibrator
    }

    private fun foreground(text: String) {
        if (Build.VERSION.SDK_INT >= 34) startForeground(NOTIFICATION_ID, notification(text), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        else startForeground(NOTIFICATION_ID, notification(text))
    }

    private fun vibrate(level: Float, frequency: Float, durationMs: Int) {
        if (level <= 0f) { vibrator.cancel(); return }
        val amplitude = (level * 255).toInt().coerceIn(1, 255)
        val effect = if (vibrator.hasAmplitudeControl()) VibrationEffect.createOneShot(durationMs.toLong().coerceAtLeast(10), amplitude)
        else VibrationEffect.createOneShot(durationMs.toLong().coerceAtLeast(10), VibrationEffect.DEFAULT_AMPLITUDE)
        vibrator.vibrate(effect)
    }

    private fun notify(text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(text))
    }

    private fun notification(text: String): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Виртуальная машина", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, if (restoreSession != null) RestoreMonitorActivity::class.java else VmActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, VmService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle("iOS 14 · iPhone 11")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Выключить", stop).build())
            .build()
    }

    companion object {
        const val ACTION_RESTORE = "dev.embervm.android.RESTORE"
        const val ACTION_START = "dev.embervm.android.START"
        const val ACTION_STOP = "dev.embervm.android.STOP"
        const val EXTRA_ARGS = "args"
        const val EXTRA_HAPTICS = "haptics"
        const val EXTRA_RENDERER = "renderer"
        const val EXTRA_PATCH_ONLY = "patchOnly"
        private const val CHANNEL = "vm"
        private const val NOTIFICATION_ID = 1

        fun stop(context: Context) {
            context.startService(Intent(context, VmService::class.java).setAction(ACTION_STOP))
        }
    }
}
