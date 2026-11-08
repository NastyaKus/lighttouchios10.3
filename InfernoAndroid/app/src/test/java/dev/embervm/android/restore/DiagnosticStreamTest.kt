package dev.embervm.android.restore

import java.io.ByteArrayOutputStream
import org.junit.Assert.*
import org.junit.Test

class DiagnosticStreamTest {
    @Test fun shortTraceIsCopiedExactly() {
        val bytes=ByteArray(1234){it.toByte()};val out=ByteArrayOutputStream()
        assertEquals(DiagnosticStream.Result(1234,false),DiagnosticStream.copy(bytes.inputStream(),out,4096))
        assertArrayEquals(bytes,out.toByteArray())
    }
    @Test fun oversizedTraceIsBoundedAndMarkedTruncated() {
        val bytes=ByteArray(5000){it.toByte()};val out=ByteArrayOutputStream()
        assertEquals(DiagnosticStream.Result(4096,true),DiagnosticStream.copy(bytes.inputStream(),out,4096))
        assertArrayEquals(bytes.copyOf(4096),out.toByteArray())
    }
    @Test fun exactlyAtLimitIsNotReportedAsTruncated() {
        val bytes=ByteArray(4096);val out=ByteArrayOutputStream()
        assertEquals(DiagnosticStream.Result(4096,false),DiagnosticStream.copy(bytes.inputStream(),out,4096))
    }
}
