// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.content.Context
import android.os.Build
import android.os.PowerManager
import android.os.SystemClock
import android.util.AtomicFile
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Bounded local telemetry, sampled on its own thread, never on the QEMU loop.
 * It measures CPU submission and queue wait, not GPU execution time. */
class GraphicsTelemetry(context: Context, private val settings: VmSettings,
                        private val selected: RendererBackend, private val safetyFallback: Boolean) {
    private val app = context.applicationContext
    private val dir = File(Prefs.root(app), "graphics").apply { mkdirs() }
    private val executor = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "ember.metrics") }
    private val request = AtomicBoolean(false)
    private val recording = AtomicBoolean(false)
    private var benchmarkThermal = 0
    private var baseline: GraphicsSample? = null
    private var rows = 0
    private val session = System.currentTimeMillis().toString()
    private val started = SystemClock.elapsedRealtimeNanos()

    fun start() {
        File(dir, "latest.csv").writeText("elapsed_ns," + GraphicsSample.names.joinToString(",") + "\n")
        active = this
        benchmarkText.value = "Записать замер · 60 с"
        executor.scheduleAtFixedRate({ runCatching { sample() }.onFailure {
            Log.e("ember.graphics", "metrics", it)
            baseline = null; recording.set(false)
            benchmarkText.value = "Ошибка записи замера"
        } }, 0, 1, TimeUnit.SECONDS)
    }
    private fun sample() {
        val now = GraphicsSample(SystemClock.elapsedRealtimeNanos(), Engine.graphicsMetrics())
        val info = Engine.graphicsInfo()
        val snapshot = metadata(info).put("counters", counters(now.values))
            .put("elapsed_ns", now.timeNs-started).put("engine_state", Engine.state)
        writeJson(File(dir, "latest.json"), snapshot)
        if (rows++ < 900) File(dir, "latest.csv").appendText(
            "${now.timeNs-started},${now.values.joinToString(",")}\n")
        if (request.getAndSet(false)) {
            baseline = now
            benchmarkThermal = thermal()
            benchmarkText.value = "Замер · 60 с осталось"
        }
        baseline?.let { first ->
            val elapsed = (now.timeNs-first.timeNs)/1e9
            if (elapsed < 60) {
                benchmarkText.value = "Замер · ${(60-elapsed).toInt().coerceAtLeast(1)} с осталось"
            } else {
                val delta = now.delta(first)
                val valid = first.values[8] == 1L && now.values[8] == 1L &&
                    selected == settings.renderer && now.values[19] == selected.nativeId.toLong() &&
                    first.values[19] == now.values[19] && delta[7] == 0L && delta[150] == 0L &&
                    delta[0] > 0 && delta[13] > 0 && Engine.state == Engine.STATE_RUNNING
                val result = metadata(info).put("kind", "ui-benchmark-v1").put("duration_s", elapsed)
                    .put("valid", valid).put("delta", counters(delta))
                    .put("guest_p50_upper_ms", GraphicsSample.percentileUpperMs(delta, .50) ?: JSONObject.NULL)
                    .put("guest_p95_upper_ms", GraphicsSample.percentileUpperMs(delta, .95) ?: JSONObject.NULL)
                    .put("guest_gap_histogram_note", "1ms bins; last >=127ms; null percentile means overflow/no frames")
                    .put("thermal_status_start", benchmarkThermal).put("thermal_status_end", thermal())
                val name = "bench-${System.currentTimeMillis()}-${settings.renderer.name}.json"
                writeJson(File(dir, name), result)
                dir.listFiles()?.filter { it.name.startsWith("bench-") && it.extension == "json" }
                    ?.sortedByDescending { it.name }?.drop(12)?.forEach { it.delete() }
                baseline = null
                recording.set(false)
                benchmarkText.value = if(valid) "Замер сохранён · повторить 60 с" else "Замер неполный · повторить 60 с"
            }
        }
    }
    private fun thermal() = app.getSystemService(PowerManager::class.java).currentThermalStatus
    private fun metadata(info: Array<String>) = JSONObject().put("schema", 1).put("session", session)
        .put("app", BuildConfig.VERSION_NAME).put("device", "${Build.MANUFACTURER} ${Build.MODEL}")
        .put("android_api", Build.VERSION.SDK_INT).put("requested", settings.renderer.name)
        .put("selected", selected.name).put("safety_fallback", safetyFallback)
        .put("fallback_reason", info[0]).put("gpu_vendor", info[1]).put("gpu_renderer", info[2]).put("gpu_version", info[3])
        .put("thermal_status", thermal()).put("cores", settings.cores).put("memory_gb", settings.memoryGb)
        .put("width", settings.displayWidth).put("height", settings.displayHeight)
        .put("tb_mb", settings.tbSizeMb).put("split_wx", settings.splitWx)
        .put("scope", "host presentation; guest GPU remains software")

    fun stop() {
        if (active === this) active = null
        executor.execute {
            if (baseline != null) benchmarkText.value = "Замер прерван: машина остановлена"
            baseline = null
            recording.set(false)
        }
        executor.shutdown()
    }
    companion object {
        @Volatile private var active: GraphicsTelemetry? = null
        private val benchmarkText = MutableStateFlow("Замер доступен после запуска iOS")
        val benchmarkStatus: StateFlow<String> = benchmarkText
        fun record60Seconds() { active?.let { if (it.recording.compareAndSet(false, true)) it.request.set(true) } }
        private fun counters(values: LongArray) = JSONObject().also { json ->
            GraphicsSample.names.forEachIndexed { i, name -> json.put(name, values[i]) }
        }
        private fun writeJson(file: File, value: JSONObject) {
            val atomic = AtomicFile(file)
            val stream = atomic.startWrite()
            try { stream.write(value.toString(2).toByteArray()); atomic.finishWrite(stream) }
            catch (e: Exception) { atomic.failWrite(stream); throw e }
        }
    }
}
