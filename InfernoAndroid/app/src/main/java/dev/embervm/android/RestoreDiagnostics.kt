// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.app.ActivityManager
import android.app.Application
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import android.os.Process
import dev.embervm.android.restore.RestorePrep
import dev.embervm.android.restore.DiagnosticStream
import java.io.File
import java.io.OutputStream
import java.io.RandomAccessFile
import java.io.PrintWriter
import java.io.StringWriter
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Captures Java failures; native crashes/LMK are reported by Android exit history. */
class EmberApplication:Application() {
    override fun onCreate() {
        super.onCreate()
        val previous=Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread,error ->
            runCatching {
                val stack=StringWriter().also { error.printStackTrace(PrintWriter(it)) }.toString().take(65536)
                File(Prefs.root(this),"java-crash.log").appendText("${System.currentTimeMillis()} pid=${Process.myPid()} thread=${thread.name}\n$stack\n")
            }
            previous?.uncaughtException(thread,error) ?: Process.killProcess(Process.myPid())
        }
    }
}

object RestoreDiagnostics {
    fun lifecycle(context:Context,message:String) {
        runCatching {
            val file=File(Prefs.root(context),"vm-lifecycle.log")
            if(file.length()>1024*1024)file.writeText(tail(file,500))
            file.appendText("${System.currentTimeMillis()} pid=${Process.myPid()} $message\n")
        }
    }
    fun exitHistory(context:Context):String {
        if(Build.VERSION.SDK_INT<30)return "Android < 11: process exit history unavailable\n"
        return runCatching {
            val manager=context.getSystemService(ActivityManager::class.java)
            manager.getHistoricalProcessExitReasons(context.packageName,0,20).joinToString("\n") { exit ->
                "time=${exit.timestamp} process=${exit.processName} pid=${exit.pid} reason=${reason(exit.reason)}(${exit.reason}) status=${exit.status} pssKiB=${exit.pss} rssKiB=${exit.rss} description=${exit.description}"
            }.ifEmpty { "Android returned no exit records" }+"\n"
        }.getOrElse { "Cannot read exit history: ${it.javaClass.simpleName}: ${it.message}\n" }
    }
    private fun reason(code:Int)=when(code) {
        ApplicationExitInfo.REASON_EXIT_SELF -> "EXIT_SELF"
        ApplicationExitInfo.REASON_SIGNALED -> "SIGNALED"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "LOW_MEMORY"
        ApplicationExitInfo.REASON_CRASH -> "JAVA_CRASH"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "NATIVE_CRASH"
        ApplicationExitInfo.REASON_ANR -> "ANR"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "USER_REQUESTED"
        else -> "OTHER"
    }
    /** Explicit allowlist: never includes the IPSW, ROM, tickets, SEP key or disk. */
    fun export(context:Context,output:OutputStream) {
        val root=Prefs.root(context);val stage=RestorePrep.staging(root)
        ZipOutputStream(output.buffered()).use { zip ->
            fun text(name:String,value:String) { zip.putNextEntry(ZipEntry(name));zip.write(value.toByteArray());zip.closeEntry() }
            fun file(name:String,source:File) {
                if(!source.isFile)return
                zip.putNextEntry(ZipEntry(name))
                RandomAccessFile(source,"r").use { input ->
                    input.seek((input.length()-2*1024*1024).coerceAtLeast(0))
                    val bytes=ByteArray(65536)
                    while(true) { val n=input.read(bytes);if(n<0)break;zip.write(bytes,0,n) }
                };zip.closeEntry()
            }
            val memory=ActivityManager.MemoryInfo().also { context.getSystemService(ActivityManager::class.java).getMemoryInfo(it) }
            val settings=Prefs.load(context)
            text("device.txt","Ember VM ${BuildConfig.APPLICATION_ID} ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})\nAndroid API ${Build.VERSION.SDK_INT}, ${Build.MANUFACTURER} ${Build.MODEL}\nABIs=${Build.SUPPORTED_ABIS.joinToString()}\nRAM total=${memory.totalMem} available=${memory.availMem} lowMemory=${memory.lowMemory}\nFree storage=${root.usableSpace}\nGuest cores=${settings.cores} RAM GiB=${settings.memoryGb} TB MiB=${settings.tbSizeMb} splitWx=${settings.splitWx}\nReport generated=${System.currentTimeMillis()}\n")
            text("android-exit-history.txt",exitHistory(context))
            val traceReport=StringBuilder()
            if(Build.VERSION.SDK_INT>=30) {
                try {
                    val exits=context.getSystemService(ActivityManager::class.java)
                        .getHistoricalProcessExitReasons(context.packageName,0,20)
                        .filter { it.processName in setOf(context.packageName+":vm",context.packageName+":gpulab",context.packageName+":gpu") && it.reason==ApplicationExitInfo.REASON_CRASH_NATIVE }
                        .take(3)
                    if(exits.isEmpty())traceReport.append("No native VM crash records available.\n")
                    for(exit in exits) {
                        val name="native-crash/${exit.timestamp}-${exit.pid}.pb"
                        var opened=false
                        try {
                            val trace=exit.traceInputStream
                            if(trace==null) { traceReport.append("$name: Android returned no trace (expired or unavailable).\n");continue }
                            zip.putNextEntry(ZipEntry(name));opened=true
                            val result=trace.use { DiagnosticStream.copy(it,zip,4*1024*1024) }
                            traceReport.append("$name: bytes=${result.bytes} truncated=${result.truncated} signal=${exit.status}\n")
                        } catch(e:Exception) { traceReport.append("$name: ${e.javaClass.simpleName}: ${e.message}\n") }
                        finally { if(opened)zip.closeEntry() }
                    }
                } catch(e:Exception) { traceReport.append("Cannot export native traces: ${e.javaClass.simpleName}: ${e.message}\n") }
            } else traceReport.append("Native exit traces require Android 11+.\n")
            text("native-traces.txt",traceReport.toString())
            for(name in listOf("restore-status.plist","restore.log","restore-error.log","vm-lifecycle.log","java-crash.log"))file(name,File(root,name))
            file("last-guest-console.log",File(stage,"guest-console.log"))
            file("restored-checkpoint.plist",File(stage,"restored.plist"))
            text("restore-checkpoint.txt",dev.embervm.android.restore.RestoreCheckpoint.diagnostics(root))
            file("last-engine.log",File(stage,"logs/engine.log"))
            file("vm-guest-console.log", Prefs.config(context).guestConsoleLog)
            file("vm-engine.log", Prefs.config(context).engineLog)
            file("gpu-lab/status.json",File(root,"gpu-lab/status.json"))
            file("gpu-lab/system-host-selftest.json",SystemGpuHostCheck.file(context))
            file("system-gpu-vm.json",File(root,"system-gpu-vm.json"))
            Prefs.systemGpuLatch(context).takeIf { it.tripped }?.let { text("system-gpu-latch.txt",it.reason+"\n") }
            File(root,"gpu-lab/runs").listFiles()?.filter { it.isDirectory && GpuLabKit.runName.matches(it.name) }
                ?.sortedByDescending { it.name }?.take(3)?.forEach { run ->
                    for(name in GpuLabReports.allowedFiles)file("gpu-lab/${run.name}/$name",File(run,name))
                }
            val gpuRun=GpuLabReports.status(context).run
            if(GpuLabKit.runName.matches(gpuRun)) {
                val live=File(context.filesDir,"gpu-lab-scratch/$gpuRun")
                file("gpu-lab/live-guest-console.log",File(live,"guest-console.log"))
                file("gpu-lab/live-engine.log",File(live,"logs/engine.log"))
            }
            File(root,"graphics").listFiles()?.filter {
                it.name in setOf("latest.json", "latest.csv") ||
                    (it.name.matches(Regex("bench-[0-9]+-(CPU|GLES3)\\.json")))
            }?.sortedByDescending { it.name }?.take(14)?.forEach { file("graphics/${it.name}", it) }
            val whitelist=setOf("status.plist","host.log","restore-error.log","guest-console.log","engine.log","vm-lifecycle.log","java-crash.log")
            File(root,"restore-attempts").listFiles()?.filter { it.isDirectory }?.sortedByDescending { it.name }?.take(3)?.forEach { dir ->
                for(name in whitelist)file("previous/${dir.name}/$name",File(dir,name))
            }
        }
    }
}
