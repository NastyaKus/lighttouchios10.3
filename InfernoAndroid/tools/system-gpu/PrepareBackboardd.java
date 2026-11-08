// SPDX-License-Identifier: GPL-3.0-or-later
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.HexFormat;
import dev.embervm.android.*;

/** Caller-owned input/output only; never bundle or distribute these Apple files. */
public final class PrepareBackboardd {
    public static void main(String[] args) throws Exception {
        if(args.length!=2)throw new IllegalArgumentException("stock-backboardd NEW-private-output");
        Path input=Path.of(args[0]),output=Path.of(args[1]);
        if(Files.size(input)!=1121392 || Files.exists(output))throw new IllegalArgumentException("Stock size or fresh output requirement");
        byte[] result=SystemGpuPreparation.INSTANCE.backboardd("18A5351d",Files.readAllBytes(input));
        Files.write(output,result,StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
        var identity=SystemMachO.INSTANCE.inspect(result);
        System.out.println("Prepared only: UUID="+identity.getUuid()+" bytes="+result.length+
            " SHA256="+HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(result))+
            " entitlements_bytes="+identity.getEntitlements().length+" signature_pages=verified execution=NOT_TESTED");
    }
}
