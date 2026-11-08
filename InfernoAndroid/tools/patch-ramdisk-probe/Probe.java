// SPDX-License-Identifier: GPL-3.0-or-later
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
public class Probe {
 static Object call(Object obj,String method,Object... args) throws Exception {
  Method m=Arrays.stream(obj.getClass().getDeclaredMethods()).filter(x->x.getName().equals(method)).findFirst().orElseThrow();m.setAccessible(true);return m.invoke(obj,args);
 }
 static Object volume(RandomAccessFile file) throws Exception {
  file.seek(0);element(file);for(int i=0;i<3;i++){long n=element(file);file.seek(file.getFilePointer()+n);}long length=element(file),base=file.getFilePointer();
  Class<?> cls=Class.forName("dev.embervm.android.restore.RestoreRamdisk$Volume");var c=cls.getDeclaredConstructor(RandomAccessFile.class,long.class,long.class);c.setAccessible(true);return c.newInstance(file,base,length);
 }
 static long element(RandomAccessFile f) throws Exception {f.readUnsignedByte();int n=f.readUnsignedByte();if(n<128)return n;long size=0;for(int i=0;i<(n&127);i++)size=(size<<8)|f.readUnsignedByte();return size;}
 static Object entry(Object volume,String path) throws Exception {
  long parent=2;Object node=null;
  for(String name:path.substring(1).split("/")){node=call(volume,"lookup",parent,name);if(node==null)throw new AssertionError("Missing "+path);parent=(long)call(node,"getId");}return node;
 }
 static boolean exists(Object volume,String path) throws Exception {
  try {entry(volume,path);return true;}catch(AssertionError missing){return false;}
 }
 static String hash(File f) throws Exception {var digest=MessageDigest.getInstance("SHA-256");try(var in=new FileInputStream(f)){byte[] b=new byte[1048576];int n;while((n=in.read(b))>0)digest.update(b,0,n);}return HexFormat.of().formatHex(digest.digest());}
 public static void main(String[] args) throws Exception {
  File original=new File(args[0]), patched=new File(args[1]);String before=hash(original);
  String job="/System/Library/LaunchDaemons/com.apple.restored_external.plist";
  long oldSize;int oldFlags;boolean umountPresent;
  try(var file=new RandomAccessFile(original,"r")){
   Object v=volume(file),e=entry(v,job),fork=call(e,"getFork");oldSize=(long)call(fork,"getSize");file.seek((long)call(e,"getAt")+41);oldFlags=file.readUnsignedByte();
   entry(v,"/usr/local/bin/restored_external"); // Verify the actual executable paths, too.
   entry(v,"/usr/bin/nvmefwupdater");
   umountPresent=exists(v,"/sbin/umount") || exists(v,"/bin/umount") || exists(v,"/usr/bin/umount");
  }
  Class<?> cls=Class.forName("dev.embervm.android.restore.RestoreRamdisk");Object instance=cls.getField("INSTANCE").get(null);
  cls.getMethod("buildPatchBoot",File.class,File.class,byte[].class).invoke(instance,original,patched,Files.readAllBytes(Path.of(args[2])));
  byte[] expected=(byte[])cls.getMethod("getDisabledRestoreMonitorJob").invoke(instance);
  byte[] storageExpected=(byte[])cls.getMethod("getStorageJob").invoke(instance);
  try(var file=new RandomAccessFile(patched,"r")) {
   Object v=volume(file),e=entry(v,job),fork=call(e,"getFork");long size=(long)call(fork,"getSize");byte[] contents=(byte[])call(v,"readFork",fork,0L,(int)size);
   if(!Arrays.equals(expected,contents))throw new AssertionError("Patched plist bytes differ");file.seek((long)call(e,"getAt")+41);int flags=file.readUnsignedByte();if(flags!=0)throw new AssertionError("Compressed flag remains");
   Object storage=entry(v,"/System/Library/LaunchDaemons/com.apple.KeyMaker.plist"),storageFork=call(storage,"getFork");
   long storageSize=(long)call(storageFork,"getSize");
   if(!Arrays.equals(storageExpected,(byte[])call(v,"readFork",storageFork,0L,(int)storageSize)))throw new AssertionError("Storage job bytes differ");
   Object daemonEntry=entry(v,"/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-01.30-006-P.bin"),daemonFork=call(daemonEntry,"getFork");
   byte[] expectedDaemon=Files.readAllBytes(Path.of(args[2]));
   if(!Arrays.equals(expectedDaemon,(byte[])call(v,"readFork",daemonFork,0L,expectedDaemon.length)) || (long)call(daemonFork,"getSize")!=expectedDaemon.length)throw new AssertionError("Updated daemon bytes differ");
   if(!before.equals(hash(original)) || original.length()!=patched.length())throw new AssertionError("Input modified or IM4P resized");
   System.out.println("{\"source_sha256\":\""+before+"\",\"im4p_bytes\":"+original.length()+",\"original_job_fork_bytes\":"+oldSize+",\"original_flags\":"+oldFlags+",\"new_job_bytes\":"+size+",\"new_flags\":"+flags+",\"input_unchanged\":true,\"actual_executable_path_verified\":true,\"job_bytes_verified\":true,\"storage_job_bytes\":"+storageSize+",\"storage_job_bytes_verified\":true,\"nvmefwupdater_path_verified\":true,\"updated_daemon_verified\":true,\"umount_executable_present\":"+umountPresent+",\"guest_boot_tested\":false}");
  }
 }
}
