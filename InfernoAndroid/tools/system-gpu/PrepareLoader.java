// SPDX-License-Identifier: GPL-3.0-or-later
import java.nio.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.HexFormat;
import dev.embervm.android.*;

/** Our executable + our dylib, never system-GPU or backboardd validation. */
public final class PrepareLoader {
    public static void main(String[] args) throws Exception {
        boolean system=args.length==4 && args[0].equals("--stock-backboardd");
        if(args.length!=(system?4:3))throw new IllegalArgumentException("[--stock-backboardd] executable own-dylib NEW-bundle");
        int offset=system?1:0;
        Path exe=Path.of(args[offset]),dylib=Path.of(args[offset+1]),output=Path.of(args[offset+2]);
        if(Files.size(exe)>2*1024*1024 || Files.size(dylib)>2*1024*1024)throw new IllegalArgumentException("Test bundle bounds");
        byte[] data=Files.readAllBytes(exe),library=Files.readAllBytes(dylib);
        byte[] signed;
        if(system) signed=SystemGpuPreparation.INSTANCE.backboardd("18A5351d",data);
        else {
            var identity=SystemMachO.INSTANCE.inspect(data);
            String digest=HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(data));
            var prepared=SystemMachO.INSTANCE.inject(data,SystemGpuPreparation.LIBRARY,identity.getUuid(),digest);
            signed=SystemCodeSigner.INSTANCE.sign(prepared);
        }
        ByteBuffer bundle=ByteBuffer.allocate(16+signed.length+library.length).order(ByteOrder.LITTLE_ENDIAN);
        bundle.put("EMBLOAD1".getBytes(java.nio.charset.StandardCharsets.US_ASCII)).putInt(signed.length).putInt(library.length).put(signed).put(library);
        Files.write(output,bundle.array(),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
        // TrustCache must hash the injected/signed executable, not the input.
        Files.write(output.resolveSibling(output.getFileName()+".executable"),signed,StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
    }
}
