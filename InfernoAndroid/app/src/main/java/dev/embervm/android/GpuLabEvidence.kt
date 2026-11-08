// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

/** Pure evidence checks, independent of Activity/service lifecycle. */
data class GpuLabHostResult(val success:Boolean,val vendor:String,val renderer:String,val version:String,
    val error:String,val commands:Long,val draws:Long,val pixels:Long,val calls:Long,val batches:Long,
    val apiDraws:Long,val shaderCompiles:Long,val presentation:GpuPresentationStats?=null)

object GpuLabEvidence {
    fun decodeNative(values:Array<String>):GpuLabHostResult {
        require(values.size==12 && values[0] in setOf("0","1")) { "Некорректный ответ GPU JNI" }
        val counters=values.drop(5).map { it.toLong().also { n -> require(n>=0) } }
        return GpuLabHostResult(values[0]=="1",values[1],values[2],values[3],values[4],
            counters[0],counters[1],counters[2],counters[3],counters[4],counters[5],counters[6])
    }
    fun verify(console:String,host:GpuLabHostResult):Int {
        require(!console.contains("panic(cpu") && !console.contains("*** EGPU-GLES2: FAIL") && !console.contains("*** EGPU-EAGL: FAIL") && !console.contains("*** EGPU-PRESENT: FAIL")) { "Ошибка гостевого GPU-теста" }
        require(host.success) { "GPU-хост: ${host.error.ifEmpty { "нет подтверждения выполнения" }}" }
        val pass=Regex("\\*\\*\\* EGPU-GLES2: PASS checks=([0-9]+) packets=([0-9]+)").find(console)
            ?: error("Нет подтверждения GLES-теста от гостя")
        val checks=pass.groupValues[1].toInt();val packets=pass.groupValues[2].toLong()
        require(checks>=40 && packets<1_000_000 && host.commands==packets+1) { "Счётчики гостя и GPU-хоста расходятся" }
        require(host.calls>=80 && host.batches>=10 && host.apiDraws>=4 && host.shaderCompiles>=5 && host.draws>=4 && host.pixels>=16384) { "GPU-хост не подтвердил шейдеры, отрисовку или чтение пикселей" }
        require(console.contains("*** EGPU-EAGL: PASS API=2 current/detach/reattach; TLS ownership verified;")) { "EAGLContext и владение между потоками не проверены" }
        require(console.contains("real compiler rejected bad shader") && console.contains("batched ten GL calls into one request")) { "Нет подтверждения compiler/error/batch тестов" }
        return checks
    }
}
