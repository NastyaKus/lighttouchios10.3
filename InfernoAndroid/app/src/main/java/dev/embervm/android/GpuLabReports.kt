// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import android.content.Context
import java.io.File
import java.io.FileOutputStream
import org.json.JSONObject

object GpuLabReports {
    data class Status(val phase:String="IDLE",val message:String="Тест ещё не запускался",val run:String="",val time:Long=0) {
        val busy get()=phase in setOf("PREPARING","WAIT_GPU","BOOTING","PRESENTING","VERIFYING","STOPPING")
    }
    fun directory(context:Context)=File(Prefs.root(context),"gpu-lab").apply { mkdirs() }
    fun run(context:Context,name:String):File {
        require(GpuLabKit.runName.matches(name));return File(directory(context),"runs/$name")
    }
    fun read(file:File):JSONObject?=runCatching {
        require(file.length() in 1..65536);JSONObject(file.readText())
    }.getOrNull()
    fun status(context:Context):Status {
        val p=read(File(directory(context),"status.json")) ?: return Status()
        val name=p.optString("run");if(!GpuLabKit.runName.matches(name))return Status()
        return Status(p.optString("phase"),p.optString("message"),name,p.optLong("time"))
    }
    fun write(context:Context,run:String,phase:String,message:String) {
        atomic(File(directory(context),"status.json"),JSONObject().put("phase",phase).put("message",message)
            .put("run",run).put("time",System.currentTimeMillis()).put("full_guest_gpu_acceleration",false))
    }
    fun atomic(file:File,value:JSONObject) {
        file.parentFile!!.mkdirs();val part=File(file.path+".part")
        try {
            FileOutputStream(part).use { it.write((value.toString(2)+"\n").toByteArray());it.fd.sync() }
            check(part.renameTo(file)) { "Не удалось сохранить ${file.name}" }
        } finally { part.delete() }
    }
    fun hostJson(h:GpuLabHostResult)=JSONObject().put("success",h.success).put("vendor",h.vendor)
        .put("renderer",h.renderer).put("version",h.version).put("error",h.error)
        .put("commands",h.commands).put("draws",h.draws).put("pixels",h.pixels)
        .put("calls",h.calls).put("batches",h.batches).put("apiDraws",h.apiDraws).put("shaderCompiles",h.shaderCompiles)
        .apply { h.presentation?.let { put("presentation",presentationJson(it)) } }
    fun presentationJson(s:GpuPresentationStats)=JSONObject().put("frames",s.frames).put("windowFrames",s.windowFrames)
        .put("generation",s.generation).put("creates",s.creates).put("losses",s.losses).put("serial",s.serial)
        .put("pixels",org.json.JSONArray(s.pixels)).put("width",s.width).put("height",s.height)
        .put("sampledPixels",s.sampledPixels).put("cpuUploadBytes",s.cpuUploadBytes)
    private fun presentation(p:JSONObject):GpuPresentationStats {
        val colors=p.getJSONArray("pixels")
        return GpuPresentationStats.decode(longArrayOf(p.getLong("frames"),p.getLong("windowFrames"),p.getLong("generation"),
            p.getLong("creates"),p.getLong("losses"),p.getLong("serial"),colors.getLong(0),colors.getLong(1),colors.getLong(2),colors.getLong(3),
            p.getLong("width"),p.getLong("height"),p.getLong("sampledPixels"),p.getLong("cpuUploadBytes")))
    }
    fun host(json:JSONObject)=GpuLabHostResult(json.optBoolean("success"),json.optString("vendor"),
        json.optString("renderer"),json.optString("version"),json.optString("error"),
        json.optLong("commands"),json.optLong("draws"),json.optLong("pixels"),json.optLong("calls"),
        json.optLong("batches"),json.optLong("apiDraws"),json.optLong("shaderCompiles"),json.optJSONObject("presentation")?.let { presentation(it) })
    fun capture(p:JSONObject):GpuDisplayCapture {
        fun colors(name:String)=p.getJSONArray(name).let { array -> (0 until array.length()).map { array.getLong(it) } }
        return GpuDisplayCapture(p.getString("run"),p.getLong("generation"),p.getLong("serial"),p.getInt("pixelCopyResult"),colors("expected"),colors("observed"))
    }
    val allowedFiles=setOf("result.json","host-result.json","host.log","guest-console.log","engine.log","run.log","display-result.json")
    fun prune(context:Context) {
        File(directory(context),"runs").listFiles()?.filter { it.isDirectory && GpuLabKit.runName.matches(it.name) }
            ?.sortedByDescending { it.name }?.drop(3)?.forEach { old ->
                // Delete only our logs, preserving any unexpected user files.
                for(name in allowedFiles){File(old,name).delete();File(old,"$name.part").delete()}
                old.delete()
            }
    }
}
