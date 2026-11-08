package dev.embervm.android

import android.app.ActivityManager
import android.content.Context
import java.io.File

/** Settings, and the few places on disk both processes agree on. */
object Prefs {
    private const val NAME = "inferno"

    fun load(context: Context): VmSettings {
        val p = context.getSharedPreferences(NAME, Context.MODE_PRIVATE)
        val d = VmSettings()
        return VmSettings(
            cores = p.getInt("cores", d.cores),
            memoryGb = p.getInt("memoryGb", d.memoryGb),
            halfResolution = p.getBoolean("halfResolution", d.halfResolution),
            network = p.getBoolean("network", d.network),
            tbSizeMb = p.getInt("tbSizeMb", d.tbSizeMb),
            splitWx = p.getBoolean("splitWx", d.splitWx),
            bootArgs = p.getString("bootArgs", d.bootArgs) ?: "",
            volumeKeys = p.getBoolean("volumeKeys", d.volumeKeys),
            haptics = p.getBoolean("haptics", d.haptics),
            showFps = p.getBoolean("showFps", d.showFps),
            renderer = RendererBackend.parse(p.getString("renderer", null)),
        )
    }

    fun save(context: Context, s: VmSettings) {
        context.getSharedPreferences(NAME, Context.MODE_PRIVATE).edit()
            .putInt("cores", s.cores)
            .putInt("memoryGb", s.memoryGb)
            .putBoolean("halfResolution", s.halfResolution)
            .putBoolean("network", s.network)
            .putInt("tbSizeMb", s.tbSizeMb)
            .putBoolean("splitWx", s.splitWx)
            .putString("bootArgs", s.bootArgs)
            .putBoolean("volumeKeys", s.volumeKeys)
            .putBoolean("haptics", s.haptics)
            .putBoolean("showFps", s.showFps)
            .putString("renderer", s.renderer.name)
            .commit()
    }

    /** Android/data/dev.embervm.android/files — reachable over USB from a computer. */
    fun root(context: Context): File = (context.getExternalFilesDir(null) ?: context.filesDir).also { it.mkdirs() }

    fun config(context: Context) = VmConfig(root(context))

    fun graphicsSafety(context: Context) = GraphicsSafety(File(context.filesDir, "graphics-session"))

    /** Where the engine's own data files are unpacked (-L). */
    fun qemuData(context: Context) = File(context.filesDir, "qemu-data")

    /** Written by :vm when the machine stops on its own terms. */
    fun exitMarker(context: Context) = File(context.filesDir, "last-exit")

    fun vmRunning(context: Context): Boolean {
        val am = context.getSystemService(ActivityManager::class.java)
        val names = setOf(context.packageName + ":vm", context.packageName + ":gpulab")
        return am.runningAppProcesses?.any { it.processName in names } == true
    }

    /** Excludes the cached EGL worker, which never owns a guest VM. */
    fun gpuLabRunning(context:Context):Boolean = context.getSystemService(ActivityManager::class.java)
        .runningAppProcesses?.any { it.processName==context.packageName+":gpulab" }==true

    /** Total RAM of the phone in GiB, rounded down. */
    fun deviceMemoryGb(context: Context): Int {
        val info = ActivityManager.MemoryInfo()
        context.getSystemService(ActivityManager::class.java).getMemoryInfo(info)
        return (info.totalMem / (1L shl 30)).toInt()
    }
}
