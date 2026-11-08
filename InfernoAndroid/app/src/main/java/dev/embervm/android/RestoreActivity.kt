// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.app.*
import android.content.*
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.*
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
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import dev.embervm.android.restore.*
import dev.embervm.android.ui.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** Shared on-disk status, because the restore runs in :vm, not in this Activity. */
object RestoreState {
    data class Snapshot(val phase:String="IDLE",val message:String="Выберите файлы",val fraction:Double=0.0) {
        val busy get()=phase in listOf("PREPARING","RESTORING")
    }
    fun read(context:Context):Snapshot = runCatching {
        val p=Plist.parse(File(Prefs.root(context),"restore-status.plist")) as? Map<*,*> ?: return@runCatching Snapshot()
        Snapshot(p["Phase"] as? String ?: "IDLE",p["Message"] as? String ?: "",(p["Fraction"] as? Number)?.toDouble() ?: 0.0)
    }.getOrDefault(Snapshot())
    fun write(context:Context,phase:String,message:String,fraction:Double=0.0) {
        val f=File(Prefs.root(context),"restore-status.plist")
        val temp=File(f.path+".part")
        temp.writeBytes(PlistXml.encode(mapOf("Phase" to phase,"Message" to message,"Fraction" to fraction.coerceIn(0.0,1.0))))
        check(temp.renameTo(f)) { "Cannot write restore status" }
    }
}

class RestorePrepService:Service() {
    companion object { @Volatile var running=false; private set }
    private var worker:Thread?=null
    private var wake:PowerManager.WakeLock?=null
    override fun onBind(intent:Intent?)=null
    override fun onStartCommand(intent:Intent?,flags:Int,startId:Int):Int {
        if(intent?.action=="cancel") { worker?.interrupt();return START_NOT_STICKY }
        if(worker?.isAlive==true)return START_NOT_STICKY
        startForeground(20,notification("Подготовка IPSW"),ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        running=true
        worker=Thread({
            var key:SepFirmware.Key?=null
            var machineLease:VmLease?=null
            try {
                require(!Prefs.vmRunning(this) && !ImportState.progress.value.running) { "Сначала остановите VM и импорт" }
                machineLease=VmLease.acquire(filesDir)
                require(!File(Prefs.root(this),"restore-activation.plist").exists()) { "Нужна проверка незавершённой активации набора" }
                key=SepFirmware.Key.parse(intent?.getStringExtra("key") ?: "")
                val ipsw=Uri.parse(intent?.getStringExtra("ipsw") ?: error("IPSW не выбран"))
                val ticket=Uri.parse(intent.getStringExtra("ticket") ?: error("Ticket не выбран"))
                val rom=Uri.parse(intent.getStringExtra("rom") ?: error("SEP ROM не выбран"))
                fun open(uri:Uri)=contentResolver.openInputStream(uri) ?: error("Файл недоступен: $uri")
                fun asset(name:String)=runCatching { assets.open("guest/$name").use { it.readBytes() } }.getOrNull()
                wake=getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"ember:prepare").apply { acquire(6*60*60*1000L) }
                RestoreState.write(this,"PREPARING","Копирование IPSW")
                RestorePrep.prepare(Prefs.root(this),RestorePrep.Input({open(ipsw)},{open(ticket)},{open(rom)},key!!,asset("inferno_patcher"),asset("inferno_fs_patcher"))) {
                    RestoreState.write(this,"PREPARING",it)
                    getSystemService(NotificationManager::class.java).notify(20,notification(it))
                }
                val marker=Plist.parse(File(RestorePrep.staging(Prefs.root(this)),"prepared.plist")) as? Map<*,*>
                val message=if(marker?.get("GuestTools")==true) "Подготовлено для 18A5351d. Восстановление ещё не запускалось."
                    else "Подготовлено без программ гостя: патчи ФС не выполнятся, набор не будет активирован."
                RestoreState.write(this,"PREPARED",message)
            } catch(e:Exception) {
                RestoreState.write(this,if(e is InterruptedException)"CANCELLED" else "FAILED",e.message ?: e.javaClass.simpleName)
            } finally {
                machineLease?.close();running=false;key?.clear();intent?.removeExtra("key");wake?.let { if(it.isHeld)it.release() };stopForeground(STOP_FOREGROUND_REMOVE);stopSelf()
            }
        },"ember.prepare").apply { start() }
        return START_NOT_STICKY
    }
    private fun notification(text:String):Notification {
        val nm=getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel("restore-prep","Подготовка IPSW",NotificationManager.IMPORTANCE_LOW))
        val open=PendingIntent.getActivity(this,20,Intent(this,RestoreActivity::class.java),PendingIntent.FLAG_IMMUTABLE)
        val cancel=PendingIntent.getService(this,21,Intent(this,RestorePrepService::class.java).setAction("cancel"),PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this,"restore-prep").setSmallIcon(R.drawable.ic_stat).setContentTitle("Ember VM").setContentText(text).setOngoing(true).setContentIntent(open)
            .addAction(Notification.Action.Builder(null,"Отменить",cancel).build()).build()
    }
    override fun onTimeout(startId:Int,fgsType:Int) { worker?.interrupt();stopSelf() }
    override fun onDestroy() { worker?.interrupt();super.onDestroy() }
}

class RestoreActivity:ComponentActivity() {
    override fun onCreate(savedInstanceState:Bundle?) { super.onCreate(savedInstanceState);setContent { InfernoTheme { RestoreScreen() } } }
}

@Composable private fun RestoreScreen() {
    val context=androidx.compose.ui.platform.LocalContext.current
    val importProgress by ImportState.progress.collectAsState()
    val scope=rememberCoroutineScope()
    var launchPending by remember { mutableStateOf(false) }
    var confirmRetry by remember { mutableStateOf(false) }
    var confirmPatches by remember { mutableStateOf(false) }
    var patchReady by remember { mutableStateOf(false) }
    var ipsw by remember { mutableStateOf<Uri?>(null) };var ticket by remember { mutableStateOf<Uri?>(null) };var rom by remember { mutableStateOf<Uri?>(null) }
    var key by remember { mutableStateOf("") };var error by remember { mutableStateOf("") }
    var state by remember { mutableStateOf(RestoreState.read(context)) }
    var space by remember { mutableStateOf(Prefs.root(context).usableSpace) }
    var console by remember { mutableStateOf("") };var running by remember { mutableStateOf(Prefs.vmRunning(context)) }
    val export=rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if(uri!=null)scope.launch {
            error=try {
                withContext(Dispatchers.IO) {
                    val output=context.contentResolver.openOutputStream(uri) ?: error("Не удалось открыть файл")
                    RestoreDiagnostics.export(context,output)
                }
                "Диагностика сохранена. Прикрепите ZIP к сообщению."
            } catch(e:Exception) { "Не удалось сохранить диагностику: ${e.message}" }
        }
    }
    fun beginRestore(patchOnly:Boolean=false) {
        if(launchPending)return
        try {
            launchPending=true
            context.startForegroundService(Intent(context,VmService::class.java).setAction(VmService.ACTION_RESTORE).putExtra(VmService.EXTRA_PATCH_ONLY,patchOnly))
            context.startActivity(Intent(context,RestoreMonitorActivity::class.java));error=""
        } catch(e:Exception) { launchPending=false;error=e.message ?: "Не удалось начать восстановление" }
    }
    LaunchedEffect(launchPending) { if(launchPending) { delay(5000);launchPending=false } }
    val pickIpsw=rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { ipsw=it }
    val pickTicket=rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { ticket=it }
    val pickRom=rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { rom=it }
    LaunchedEffect(Unit) {
        while(true) {
            withContext(Dispatchers.IO) {
                state=RestoreState.read(context);space=Prefs.root(context).usableSpace;running=Prefs.vmRunning(context)
                val root=Prefs.root(context);val stage=RestorePrep.staging(root)
                patchReady=!running && RestoreCheckpoint.canPatch(root)
                console="Ошибка восстановления:\n"+tail(File(root,"restore-error.log"),30)+
                    "\nХост:\n"+tail(File(root,"restore.log"),25)+
                    "\nПоследний журнал движка:\n"+tail(File(stage,"logs/engine.log"),30)+
                    "\nПоследняя консоль гостя (может быть от прошлой попытки):\n"+tail(File(stage,"guest-console.log"),30)
            };delay(1000)
        }
    }
    val marker=File(RestorePrep.staging(Prefs.root(context)),"prepared.plist")
    val busy=state.busy && (running || RestorePrepService.running)
    val valid=runCatching { SepFirmware.Key.parse(key).clear() }.isSuccess
    Column(Modifier.fillMaxSize().background(Palette.background).safeDrawingPadding().verticalScroll(rememberScrollState()).padding(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
        Text("Ember VM · восстановление",style=MaterialTheme.typography.headlineSmall,color=Palette.Text)
        Text("iPhone 11 (iPhone12,1), iOS 14.0 beta 5, сборка 18A5351d. Все файлы выбираются с телефона.",color=Palette.Muted)
        Panel {
            OutlinedButton(onClick={pickIpsw.launch(arrayOf("*/*"))},enabled=!busy) { Text(if(ipsw==null)"Выбрать IPSW" else "IPSW выбран") }
            Text("Оригинальный IPSW iPhone12,1 18A5351d с CDN Apple; другие сборки отклоняются.",color=Palette.Muted)
            OutlinedButton(onClick={pickTicket.launch(arrayOf("*/*"))},enabled=!busy) { Text(if(ticket==null)"Выбрать ticket.shsh2" else "ticket выбран") }
            Text("XML ticket.shsh2 из File Setup Inferno; нужен ApImg4Ticket.",color=Palette.Muted)
            OutlinedButton(onClick={pickRom.launch(arrayOf("*/*"))},enabled=!busy) { Text(if(rom==null)"Выбрать SEP ROM" else "SEP ROM выбран") }
            Text("AppleSEPROM-Cebu-B1 из руководства File Setup Inferno.",color=Palette.Muted)
            OutlinedTextField(value=key,onValueChange={key=it},label={Text("SEP IV + key: 96 hex")},visualTransformation=PasswordVisualTransformation(),enabled=!busy,modifier=Modifier.fillMaxWidth())
            Text("IV и AES-256 key для SEP этой сборки — The Apple Wiki. Ключ не сохраняется.",color=Palette.Muted)
        }
        Text("Свободно: ${"%.1f".format(space.toDouble()/(1L shl 30))} ГиБ. Для подготовки нужно ≥15 ГиБ.",color=Palette.Text)
        Text("Новый набор создаётся отдельно. Текущий набор заменяется после завершения и остановки VM; его копия остаётся в restore-backup-*.",color=Palette.Muted)
        Button(onClick={
            try {
                context.startForegroundService(Intent(context,RestorePrepService::class.java).putExtra("ipsw",ipsw.toString()).putExtra("ticket",ticket.toString()).putExtra("rom",rom.toString()).putExtra("key",key));key="";error=""
            } catch(e:Exception) { error=e.message ?: "Не удалось запустить подготовку" }
        },enabled=!busy && !running && !importProgress.running && ipsw!=null && ticket!=null && rom!=null && valid && space>=RestorePrep.MIN_SPACE) { Text("Подготовить набор") }
        val retry=File(RestorePrep.staging(Prefs.root(context)),"guest-console.log").exists() ||
            File(RestorePrep.staging(Prefs.root(context)),"restore-running").exists()
        if(patchReady) {
            Button(onClick={confirmPatches=true},enabled=!busy && !running && !launchPending && !importProgress.running) {
                Text("Завершить патчи")
            }
            Text("Запись iOS подтверждена. Патчи выполнятся в новом запуске ramdisk без очистки дисков и без повторной передачи системы.",color=Palette.Muted)
        }
        Button(onClick={ if(retry || patchReady)confirmRetry=true else beginRestore() },
            enabled=!busy && !running && !launchPending && marker.isFile && !importProgress.running) {
            Text(if(launchPending)"Запуск…" else if(retry)"Повторить восстановление" else "Начать восстановление")
        }
        OutlinedButton(onClick={export.launch("EmberVM-diagnostics.zip")}) { Text("Сохранить диагностику") }
        if(running)OutlinedButton(onClick={ context.startActivity(Intent(context,RestoreMonitorActivity::class.java)) }) { Text("Экран гостя") }
        if(busy)TextButton(onClick={
            if(running)VmService.stop(context) else context.startService(Intent(context,RestorePrepService::class.java).setAction("cancel"))
        }) { Text("Отменить") }
        if(error.isNotEmpty())Text(error,color=Palette.Amber)
        Text("${state.phase}: ${state.message}",color=Palette.Text)
        LinearProgressIndicator(progress={state.fraction.toFloat()},modifier=Modifier.fillMaxWidth())
        if(!busy && !running && !RestorePrepService.running && File(Prefs.root(context),"restore-activation.plist").exists()) {
            OutlinedButton(onClick={
                try { InstallKit.recover(Prefs.root(context));RestoreState.write(context,"CANCELLED","Предыдущий набор возвращён. Подготовьте новый набор заново.") }
                catch(e:Exception) { error=e.message ?: "Не удалось вернуть набор" }
            }) { Text("Вернуть предыдущий набор") }
        }
        if(!busy && !running && !RestorePrepService.running && File(Prefs.root(context),"restore-prep.lock").exists()) {
            OutlinedButton(onClick={
                File(Prefs.root(context),"restore-prep.lock").delete()
                marker.delete();RestoreState.write(context,"CANCELLED","Подготовка была прервана. Подготовьте набор заново.")
            }) { Text("Сбросить прерванную подготовку") }
        }
        if(state.busy && !busy)Text("Процесс был прерван. Сохраните диагностику; готовность iOS не подтверждена.",color=Palette.Amber)
        TextButton(onClick={
            runCatching { context.startActivity(Intent(Intent.ACTION_VIEW,Uri.parse("https://chefkiss.dev/guides/inferno/file-setup/"))) }.onFailure { error="Не удалось открыть браузер" }
        }) { Text("Открыть File Setup Inferno") }
        TextButton(onClick={
            runCatching { context.startActivity(Intent(Intent.ACTION_VIEW,Uri.parse("https://theapplewiki.com/wiki/Keys:AzulSeed_18A5351d_(iPhone12,1)"))) }.onFailure { error="Не удалось открыть браузер" }
        }) { Text("Открыть страницу ключей 18A5351d") }
        Text("Справка: ChefKiss File Setup · The Apple Wiki. Бинарные файлы Apple в APK не входят. Восстановление на Android пока экспериментальное.",color=Palette.Muted)
        SelectionContainer { Text(console,color=Palette.Muted,style=MaterialTheme.typography.bodySmall) }
    }
    if(confirmRetry)AlertDialog(
        onDismissRequest={confirmRetry=false},
        title={Text("Повторить восстановление?")},
        text={Text("Рабочие диски незавершённой попытки будут очищены и восстановление начнётся сначала. Копия IPSW, подготовленные компоненты и предыдущие журналы сохраняются. Активный набор не затрагивается.")},
        confirmButton={TextButton(onClick={confirmRetry=false;beginRestore()}) { Text("Повторить") }},
        dismissButton={TextButton(onClick={confirmRetry=false}) { Text("Отмена") }}
    )
    if(confirmPatches)AlertDialog(
        onDismissRequest={confirmPatches=false},
        title={Text("Завершить патчи файловой системы?")},
        text={Text("Будет загружен ramdisk с патчером. Записанная iOS и состояние SEP сохраняются; повторного восстановления нет. Патчер изменит кэш и настройки служб в рабочем наборе. Сначала закройте предыдущую VM.")},
        confirmButton={TextButton(onClick={confirmPatches=false;beginRestore(true)}) { Text("Запустить патчи") }},
        dismissButton={TextButton(onClick={confirmPatches=false}) { Text("Отмена") }}
    )

}
