// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

/** A mobile uid with denied raw-device opens is weaker than an App Store sandbox. */
object GpuBrokerEvidence {
    private val pass=Regex("\\*\\*\\* EGPU-BROKER: PASS uid=501 gid=501 forwarded=([0-9]+) replayed=0 child_exit=0 cleanup=1(?:\\r*\\n|$)")
    fun finished(console:String):Boolean=pass.containsMatchIn(console)
    fun verify(console:String,hostCommands:Long) {
        require(!console.contains("*** EGPU-BROKER: FAIL") && !console.contains("*** EGPU-BROKER-CLIENT: FAIL")) { "Ошибка гостевого GPU-брокера" }
        require(Regex("\\*\\*\\* EGPU-BROKER-CLIENT: uid=501 gid=501 raw_read_denied=(1|13) raw_write_denied=(1|13)(?:\\r*\\n|$)").containsMatchIn(console)) {
            "Клиент не подтвердил mobile uid/gid и отказ чтения/записи raw-диска"
        }
        require(console.contains("*** EGPU-BROKER: authenticated uid=501 gid=501")) { "Брокер не подтвердил подключение mobile" }
        val match=pass.find(console) ?: error("Брокер не подтвердил завершение клиента и очистку")
        require(hostCommands>0 && match.groupValues[1].toLong()==hostCommands) { "Счётчики брокера и GPU-хоста расходятся" }
    }
}
