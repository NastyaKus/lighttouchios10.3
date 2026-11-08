// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android

import java.io.Closeable
import java.io.File
import java.io.RandomAccessFile
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException

/** Kernel-owned cross-process exclusion, automatically released after SIGKILL. */
class VmLease private constructor(private val path:File,private val file:RandomAccessFile,private val lock:FileLock):Closeable {
    private var closed=false
    override fun close()=synchronized(owned) {
        if(closed)return
        closed=true
        try { try { lock.release() } finally { file.close() } } finally { owned.remove(path) }
    }
    companion object {
        // POSIX record locks belong to a process: closing ANY descriptor for
        // the same inode can drop the first owner's lock. Reject local rivals
        // before opening another descriptor, not just after tryLock throws.
        private val owned=mutableMapOf<File,VmLease>()
        fun tryAcquire(privateDirectory:File):VmLease?=synchronized(owned) {
            val path=File(privateDirectory,"vm-owner.lock").canonicalFile
            if(owned.containsKey(path))return null
            val file=RandomAccessFile(path,"rw")
            try {
                val lock=try { file.channel.tryLock() } catch(_:OverlappingFileLockException) { null }
                if(lock==null){file.close();return null}
                return VmLease(path,file,lock).also { owned[path]=it }
            } catch(e:Exception){file.close();throw e}
        }
        fun acquire(privateDirectory:File)=tryAcquire(privateDirectory)
            ?: error("Другая VM, восстановление или GPU-тест ещё работают")
    }
}
