// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import dev.embervm.android.ui.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class GpuLabActivity:ComponentActivity() {
    override fun onCreate(savedInstanceState:Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { InfernoTheme { Screen() } }
    }
    private data class Snapshot(val status:GpuLabReports.Status=GpuLabReports.Status(),val occupied:Boolean=false,
        val missing:List<String> = emptyList(),val result:String="",val console:String="",val free:Long=0)
    @Composable private fun Screen() {
        var snap by remember { mutableStateOf(Snapshot()) }
        var exporting by remember { mutableStateOf(false) }
        val scope=rememberCoroutineScope()
        val preview=remember { GpuPreviewController(this) }
        val previewMessage by preview.message.collectAsState()
        DisposableEffect(preview) { preview.bind();onDispose { preview.close() } }
        val hostCheck=remember { SystemGpuHostCheck(this) }
        val hostState by hostCheck.state.collectAsState()
        DisposableEffect(hostCheck) { onDispose { hostCheck.close() } }
        LaunchedEffect(snap.status.run,snap.status.busy) { preview.currentRun=snap.status.run;preview.captureEnabled=snap.status.busy }
        val export=rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
            if(uri!=null)scope.launch {
                exporting=true
                val error=withContext(Dispatchers.IO) { runCatching {
                    contentResolver.openOutputStream(uri)?.use { RestoreDiagnostics.export(this@GpuLabActivity,it) }
                        ?: error("Не удалось открыть файл")
                }.exceptionOrNull() }
                exporting=false
                Toast.makeText(this@GpuLabActivity,error?.message ?: "Диагностика сохранена",Toast.LENGTH_LONG).show()
            }
        }
        LaunchedEffect(Unit) {
            while(true) {
                snap=withContext(Dispatchers.IO) {
                    val occupied=Prefs.vmRunning(this@GpuLabActivity)
                    val raw=GpuLabReports.status(this@GpuLabActivity)
                    val status=if(raw.busy && !Prefs.gpuLabRunning(this@GpuLabActivity) && System.currentTimeMillis()-raw.time>5000)
                        raw.copy(phase="INTERRUPTED",message="Тестовый процесс прерван. Сохраните диагностику; тест можно повторить.") else raw
                    val dir=raw.run.takeIf { GpuLabKit.runName.matches(it) }?.let { GpuLabReports.run(this@GpuLabActivity,it) }
                    Snapshot(status,occupied,GpuLabKit.missing(Prefs.config(this@GpuLabActivity)),
                        dir?.let { tail(File(it,"result.json"),100,65536) }.orEmpty(),
                        dir?.let { report ->
                            val saved=File(report,"guest-console.log")
                            val live=File(filesDir,"gpu-lab-scratch/${raw.run}/guest-console.log")
                            tail(if(live.isFile)live else saved,16)
                        }.orEmpty(),filesDir.usableSpace)
                }
                delay(1000)
            }
        }
        Column(Modifier.fillMaxSize().background(Palette.background).safeDrawingPadding()
            .verticalScroll(rememberScrollState()).padding(20.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
            Text("Ember VM · GPU-стенд",style=MaterialTheme.typography.headlineLarge)
            AndroidView(factory={ SurfaceView(it).also { view -> preview.attach(view) } },
                modifier=Modifier.fillMaxWidth().height(210.dp))
            Text(previewMessage,color=Palette.Muted)
            Panel {
                Text("Системный GPU-хост",style=MaterialTheme.typography.titleLarge)
                Text("Постоянный GPU-процесс из будущего ускорения iOS выполнит на драйвере телефона полный кадр 828×1792: шейдеры, текстуру IOSurface, общие контексты, чтение 6 МиБ и ${SystemGpuSelfTest.FRAMES} выводов на экран ниже. iOS и диски не нужны, тест занимает несколько секунд.")
                Text("Это проверка хоста и драйвера, а не ускорение iOS.",color=Palette.Muted)
            }
            AndroidView(factory={ SurfaceView(it).also { view -> hostCheck.attach(view) } },
                modifier=Modifier.fillMaxWidth().height(210.dp))
            Button(onClick={ hostCheck.start() },enabled=!hostState.busy && !snap.status.busy && !exporting,
                modifier=Modifier.fillMaxWidth()) { Text("Проверить системный GPU-хост") }
            Text(hostState.message,color=if(hostState.message.startsWith("PASSED"))Palette.Green
                else if(hostState.message.startsWith("FAILED"))Palette.Red else Palette.Muted)
            if(hostState.busy)LinearProgressIndicator(Modifier.fillMaxWidth())
            if(hostState.result.isNotBlank())Panel {
                Text("Результат проверки хоста",style=MaterialTheme.typography.titleMedium)
                SelectionContainer { Text(hostState.result,style=Mono) }
            }
            Button(onClick={
                runCatching { startForegroundService(Intent(this@GpuLabActivity,GpuLabService::class.java)
                    .setAction(GpuLabService.START).putExtra(GpuLabService.EXTRA_BROKER,true)) }
                    .onFailure { Toast.makeText(this@GpuLabActivity,it.message,Toast.LENGTH_LONG).show() }
            },enabled=!snap.occupied && !snap.status.busy && snap.missing.isEmpty() && snap.free>=GpuLabKit.MIN_SPACE && !exporting,
                modifier=Modifier.fillMaxWidth()) { Text("Проверить GPU через брокер") }
            Button(onClick={
                runCatching { startForegroundService(Intent(this@GpuLabActivity,GpuLabService::class.java)
                    .setAction(GpuLabService.START).putExtra(GpuLabService.EXTRA_PRESENT,true)) }
                    .onFailure { Toast.makeText(this@GpuLabActivity,it.message,Toast.LENGTH_LONG).show() }
            },enabled=!snap.occupied && !snap.status.busy && snap.missing.isEmpty() && snap.free>=GpuLabKit.MIN_SPACE && !exporting,
                modifier=Modifier.fillMaxWidth()) { Text("Показать GPU-кадр гостя") }
            Panel {
                Text("Команды iOS → GPU телефона",style=MaterialTheme.typography.titleLarge)
                Text("Отдельная тестовая iOS проверит шейдеры, текстуры и EAGLContext на драйвере телефона. Это ещё не ускорение интерфейса iOS или GarageBand.")
                Text("В тесте вывода появятся цветные области и движущийся треугольник. Оставьте этот экран открытым до результата.")
                Text("Тест брокера запускает отдельный клиент mobile: он проверит отказ доступа к диску гостя и передаст команды через IPC. Это ещё не sandbox обычного приложения.",color=Palette.Muted)
                Text("Тест использует временные диски. Рабочий образ и настройки iOS сохраняются. Нужно 512 МиБ свободного места; ожидание результата — до 5 минут.",color=Palette.Muted)
                Text("Свободно: ${snap.free/(1024*1024)} МиБ")
                if(snap.missing.isNotEmpty())Text("Не хватает компонентов подготовленного набора 18A5351d:\n${snap.missing.joinToString("\n")}",color=Palette.Amber)
            }
            Button(onClick={
                runCatching { startForegroundService(Intent(this@GpuLabActivity,GpuLabService::class.java).setAction(GpuLabService.START)) }
                    .onFailure { Toast.makeText(this@GpuLabActivity,it.message,Toast.LENGTH_LONG).show() }
            },enabled=!snap.occupied && !snap.status.busy && snap.missing.isEmpty() && snap.free>=GpuLabKit.MIN_SPACE && !exporting,
                modifier=Modifier.fillMaxWidth()) { Text("Проверить гостевые GPU-команды") }
            if(snap.occupied && snap.status.busy)OutlinedButton(onClick={
                startService(Intent(this@GpuLabActivity,GpuLabService::class.java).setAction(GpuLabService.STOP))
            },modifier=Modifier.fillMaxWidth()) { Text("Остановить тест") }
            if(snap.occupied && !snap.status.busy)Text("Сначала остановите виртуальную машину и дождитесь завершения её процесса.",color=Palette.Amber)
            Text("${snap.status.phase}: ${snap.status.message}")
            if(snap.status.busy)LinearProgressIndicator(Modifier.fillMaxWidth())
            OutlinedButton(onClick={ export.launch("EmberVM-GPU-diagnostics.zip") },enabled=!exporting,
                modifier=Modifier.fillMaxWidth()) { Text(if(exporting)"Сохранение…" else "Сохранить диагностику ZIP") }
            if(snap.result.isNotBlank())Panel {
                Text("Результат теста",style=MaterialTheme.typography.titleMedium)
                SelectionContainer { Text(snap.result,style=Mono) }
            }
            if(snap.console.isNotBlank())Panel {
                Text("Консоль тестового гостя",style=MaterialTheme.typography.titleMedium)
                SelectionContainer { Text(snap.console,style=Mono) }
            }
        }
    }
}
