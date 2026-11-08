// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.os.Bundle
import android.content.Intent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
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
import dev.embervm.android.restore.RestorePrep
import dev.embervm.android.ui.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.io.File

/** The preview must share :vm with the engine; a Surface cannot cross processes. */
class RestoreMonitorActivity:ComponentActivity() {
    override fun onCreate(savedInstanceState:Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContent { InfernoTheme {
            var state by remember { mutableStateOf(RestoreState.read(this)) }
            var console by remember { mutableStateOf("") }
            var observedRun by remember { mutableStateOf(false) }
            LaunchedEffect(state.phase) {
                if(state.phase=="RESTORING")observedRun=true
                if(observedRun && state.phase in listOf("FAILED","CANCELLED","INCOMPLETE","COMPLETE")) {
                    // Result UI must survive the deliberate end of the :vm process.
                    startActivity(Intent(this@RestoreMonitorActivity,RestoreActivity::class.java)
                        .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP))
                    finish()
                }
            }
            LaunchedEffect(Unit) {
                while(true) { withContext(Dispatchers.IO) {
                    state=RestoreState.read(this@RestoreMonitorActivity)
                    val cfg=VmConfig(RestorePrep.staging(Prefs.root(this@RestoreMonitorActivity)))
                    console=tail(cfg.guestConsoleLog,50)
                };delay(1000) }
            }
            Column(Modifier.fillMaxSize().background(Palette.background).safeDrawingPadding().verticalScroll(rememberScrollState()).padding(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
                Text("Ember VM · восстановление",color=Palette.Text,style=MaterialTheme.typography.headlineSmall)
                Text(state.message,color=Palette.Text)
                LinearProgressIndicator(progress={state.fraction.toFloat()},modifier=Modifier.fillMaxWidth())
                AndroidView(factory={ context -> SurfaceView(context).apply {
                    holder.addCallback(object:SurfaceHolder.Callback {
                        override fun surfaceCreated(h:SurfaceHolder) { Engine.setSurface(h.surface) }
                        override fun surfaceChanged(h:SurfaceHolder,format:Int,width:Int,height:Int) { Engine.setSurface(h.surface) }
                        override fun surfaceDestroyed(h:SurfaceHolder) { Engine.setSurface(null) }
                    })
                } },modifier=Modifier.height(320.dp).aspectRatio(414f/896f))
                TextButton(onClick={VmService.stop(this@RestoreMonitorActivity)}) { Text("Остановить восстановление") }
                Text("Последняя сохранённая консоль гостя",color=Palette.Muted)
                SelectionContainer { Text(console,color=Palette.Muted,style=MaterialTheme.typography.bodySmall) }
            }
        } }
    }
}
