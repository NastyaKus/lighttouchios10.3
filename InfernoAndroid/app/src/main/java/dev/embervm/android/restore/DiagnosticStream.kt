// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android.restore

import java.io.InputStream
import java.io.OutputStream

/** Bounded native tombstone export; never reads an unbounded system trace. */
object DiagnosticStream {
    data class Result(val bytes:Int,val truncated:Boolean)
    fun copy(input:InputStream,output:OutputStream,limit:Int):Result {
        require(limit>0)
        val buffer=ByteArray(minOf(65536,limit));var count=0
        while(count<limit) {
            val n=input.read(buffer,0,minOf(buffer.size,limit-count))
            if(n<0)return Result(count,false)
            if(n==0) { val one=input.read();if(one<0)return Result(count,false);output.write(one);count++;continue }
            output.write(buffer,0,n);count+=n
        }
        return Result(count,input.read()!=-1)
    }
}
