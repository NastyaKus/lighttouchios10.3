// SPDX-License-Identifier: GPL-3.0-or-later
import java.io.File;
import java.nio.file.Files;
public class BuildRamdisk {
 public static void main(String[] args) throws Exception {
  var cls=Class.forName("dev.embervm.android.restore.RestoreRamdisk");var obj=cls.getField("INSTANCE").get(null);
  cls.getMethod("build",File.class,File.class,byte[].class,byte[].class).invoke(obj,new File(args[0]),new File(args[1]),Files.readAllBytes(new File(args[3]).toPath()),Files.readAllBytes(new File(args[4]).toPath()));
  cls.getMethod("buildPatchBoot",File.class,File.class,byte[].class).invoke(obj,new File(args[1]),new File(args[2]),Files.readAllBytes(new File(args[3]).toPath()));
 }
}
