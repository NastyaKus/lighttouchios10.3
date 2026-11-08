// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

data class GpuPresentationStats(val frames:Long,val windowFrames:Long,val generation:Long,val creates:Long,
    val losses:Long,val serial:Long,val pixels:List<Long>,val width:Int,val height:Int,val sampledPixels:Long,val cpuUploadBytes:Long) {
    // Native samples use GL bottom-left origin, Bitmap uses top-left origin.
    val bitmapCorners get()=listOf(pixels[2],pixels[3],pixels[0],pixels[1])
    companion object {
        fun decode(v:LongArray):GpuPresentationStats {
            require(v.size==14 && v.all { it>=0 }) { "Некорректная статистика present" }
            require(v[0]<=1_000_000 && v[5]<=1_000_000)
            require(v[1]<=v[0] && v[10]<=512 && v[11]<=512 && v[12]==v[0]*4 && v[13]==0L)
            require(v.slice(6..9).all { it<=0xffffffffL })
            if(v[0]>0)require(v[10]>0 && v[11]>0 && v[5]>0)
            return GpuPresentationStats(v[0],v[1],v[2],v[3],v[4],v[5],v.slice(6..9),v[10].toInt(),v[11].toInt(),v[12],v[13])
        }
    }
}
data class GpuDisplayCapture(val run:String,val generation:Long,val serial:Long,val result:Int,
    val expected:List<Long>,val observed:List<Long>)

object GpuPresentationEvidence {
    fun rgba(argb:Int):Long=((argb ushr 16 and 255).toLong() or ((argb ushr 8 and 255).toLong() shl 8) or
        ((argb and 255).toLong() shl 16) or ((argb ushr 24 and 255).toLong() shl 24))
    fun pixelsMatch(expected:List<Long>,observed:List<Long>):Boolean = expected.size==4 && observed.size==4 &&
        expected.zip(observed).all { (a,b) -> a in 0..0xffffffffL && b in 0..0xffffffffL &&
            (0..3).all { byte -> kotlin.math.abs(((a ushr (byte*8)) and 255)-((b ushr (byte*8)) and 255))<=2 } }
    fun verify(console:String,stats:GpuPresentationStats,run:String,capture:GpuDisplayCapture) {
        require(stats.cpuUploadBytes==0L && stats.sampledPixels==stats.frames*4) { "Некорректный учёт копирования кадра" }
        require(!console.contains("*** EGPU-PRESENT: FAIL")) { "Гостевой present-тест завершился с ошибкой" }
        val pass=Regex("\\*\\*\\* EGPU-PRESENT: PASS frames=([0-9]+) window_frames=([0-9]+) checks=([0-9]+)").find(console)
            ?: error("Нет подтверждения present от гостя")
        require(pass.groupValues[1].toLong()==stats.frames && pass.groupValues[2].toLong()==stats.windowFrames &&
            stats.frames>=60 && stats.windowFrames==stats.frames && pass.groupValues[3].toInt()>=190) { "Не подтверждены 60 кадров в Android Surface" }
        require(capture.run==run && capture.generation==stats.generation && stats.generation>0 && capture.serial in 1..stats.serial) { "PixelCopy относится к другому запуску или Surface" }
        require(capture.result==0 && capture.expected==stats.bitmapCorners && pixelsMatch(capture.expected,capture.observed)) { "Пиксели Android Surface не совпали с кадром гостя" }
    }
}
