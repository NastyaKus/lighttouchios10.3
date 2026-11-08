// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android.restore

import android.os.ParcelFileDescriptor
import java.io.Closeable
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.EOFException
import java.net.SocketTimeoutException

object NativeUnix {
    init { System.loadLibrary("inferno_jni") }
    @JvmStatic external fun nativeListenUnix(path:String):Int
    @JvmStatic external fun nativeAccept(fd:Int,timeoutMs:Int):Int
    @JvmStatic external fun nativePoll(fd:Int,timeoutMs:Int):Boolean
    @JvmStatic external fun nativeShutdown(fd:Int)

    class Listener(path:File):Closeable {
        private val socketPath=path
        private val descriptor=ParcelFileDescriptor.adoptFd(nativeListenUnix(path.path).also { require(it>=0) { "bind/listen Unix socket failed ($it)" } })
        fun accept(timeoutMs:Int=1000):UsbLink? {
            val fd=nativeAccept(descriptor.fd,timeoutMs)
            if(fd == -2)return null
            require(fd>=0) { "Unix accept failed ($fd)" }
            return Link(ParcelFileDescriptor.adoptFd(fd))
        }
        override fun close() { runCatching { nativeShutdown(descriptor.fd) };descriptor.close();socketPath.delete() }
    }
    private class Link(private val descriptor:ParcelFileDescriptor):UsbLink {
        private val input=FileInputStream(descriptor.fileDescriptor)
        private val output=FileOutputStream(descriptor.fileDescriptor)
        @Volatile private var closed=false
        override fun read(count:Int,timeoutMs:Long):ByteArray {
            val end=System.nanoTime()/1_000_000+timeoutMs;val bytes=ByteArray(count);var at=0
            while(at<count) {
                if(closed || Thread.currentThread().isInterrupted)throw EOFException("Unix socket closed")
                val wait=end-System.nanoTime()/1_000_000
                if(wait<=0)throw SocketTimeoutException("Unix socket read")
                if(!nativePoll(descriptor.fd,minOf(wait,1000).toInt()))continue
                val n=input.read(bytes,at,count-at);if(n<=0)throw EOFException("Guest disconnected");at+=n
            }
            return bytes
        }
        override fun write(bytes:ByteArray) { if(closed)throw EOFException();output.write(bytes) }
        override fun close() { if(closed)return;closed=true;runCatching { nativeShutdown(descriptor.fd) };descriptor.close() }
    }
}
