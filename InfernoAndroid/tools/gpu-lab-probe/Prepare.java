// SPDX-License-Identifier: GPL-3.0-or-later
import dev.embervm.android.GpuLabKit;
import dev.embervm.android.VmConfig;
import dev.embervm.android.restore.RestoreRamdisk;
import java.io.File;
import java.nio.file.Files;
import kotlin.Unit;

/** Runs the same disposable-kit preparation as Android, using owned inputs. */
public class Prepare {
    public static void main(String[] args) throws Exception {
        byte[] probe=Files.readAllBytes(new File(args[2]).toPath());
        byte[] library=Files.readAllBytes(new File(args[3]).toPath());
        GpuLabKit.Kit kit=GpuLabKit.INSTANCE.prepare(new VmConfig(new File(args[0])),new File(args[1]),(stock,out) -> {
            RestoreRamdisk.INSTANCE.buildGpuProbe(stock,out,probe,library);return Unit.INSTANCE;
        },note -> { System.out.println(note);return Unit.INSTANCE; });
        System.out.println("PREPARED scratch root fingerprint="+kit.getRootFingerprint());
    }
}
