// SPDX-License-Identifier: GPL-3.0-or-later
package dev.embervm.android;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;

/** Desktop JVM fixture for the exact JNI symbols; not an Android runtime test. */
public final class GpuHostNative {
    static { System.loadLibrary("ember_guest_gpu_probe"); }
    public static native String[] nativeRun(String image,String log,int timeout);
    public static native void nativeCancel();
    public static native void nativePresentationMode(boolean required);
    public static native long[] nativePresentationStats();
    public static void main(String[] args) throws Exception {
        if(args.length==4 && args[3].equals("cancel"))new Thread(() -> {
            try { Thread.sleep(200);nativeCancel(); } catch(InterruptedException e) { throw new RuntimeException(e); }
        }).start();
        nativePresentationMode(false);
        String[] result=nativeRun(args[0],args[1],180000);
        Files.writeString(Path.of(args[2]+".stats"),java.util.Arrays.toString(nativePresentationStats()));
        StringBuilder text=new StringBuilder();
        for(String value:result)text.append(Base64.getEncoder().encodeToString(value.getBytes(java.nio.charset.StandardCharsets.UTF_8))).append('\n');
        Files.writeString(Path.of(args[2]),text.toString());
        System.exit(result[0].equals("1")?0:1);
    }
}
