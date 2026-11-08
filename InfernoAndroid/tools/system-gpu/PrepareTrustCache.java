// SPDX-License-Identifier: GPL-3.0-or-later
import java.nio.file.*;
import java.util.*;
import dev.embervm.android.*;
import dev.embervm.android.restore.*;

/** Caller-owned scratch IM4P only; no AP ticket or engine patch is changed. */
public final class PrepareTrustCache {
    public static void main(String[] args) throws Exception {
        if(args.length<3)throw new IllegalArgumentException("stock-rtsc NEW-rtsc executable-or-dylib...");
        byte[] source=Files.readAllBytes(Path.of(args[0]));var additions=new ArrayList<SystemTrustCache.Entry>();
        for(int i=2;i<args.length;i++) {
            Path file=Path.of(args[i]);if(Files.size(file)>64L*1024*1024)throw new IllegalArgumentException("Mach-O size");
            additions.addAll(SystemTrustCache.INSTANCE.codeDirectoryHashes(Files.readAllBytes(file)));
        }
        int before=SystemTrustCache.INSTANCE.decode(Img4.INSTANCE.readIM4P(source).getData()).getEntries().size();
        byte[] result=SystemTrustCache.INSTANCE.augmentIM4P(source,additions);
        int after=SystemTrustCache.INSTANCE.decode(Img4.INSTANCE.readIM4P(result).getData()).getEntries().size();
        Files.write(Path.of(args[1]),result,StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
        System.out.println("TrustCache v1 before="+before+" after="+after+" new="+(after-before)+" engine_changes=0 AP_ticket_changes=0");
    }
}
