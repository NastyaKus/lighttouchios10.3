#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a read-only helper against the already-built pinned research block layer.

--lab is the prior Inferno Linux research workspace, --out must be writable.
This never changes/rebuilds the Android QEMU engine. External source licences
remain applicable: Inferno-current/APFS and QEMU are AGPL/GPL respectively.
"""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--lab',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();lab=a.lab.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
recipe=json.loads((lab/'artifacts/fs-patcher-legacyblock-build-command.json').read_text())
build=lab/'build/inferno';source=lab/'src/Inferno-current/fs/apfs.c'
# A bare APFS container (OS DMG) has no GPT. Validate NXSB and retain the same
# upstream bounded APFS parser. An installed GPT disk still uses its old path.
extra=r'''
ApfsFile *ember_apfs_open_container(BlockBackend *,const char *,Error **);
ApfsFile *ember_apfs_open_container(BlockBackend *blk,const char *path,Error **errp) {
    ApfsContext ctx={.dev.blk=blk};uint8_t header[APFS_BLOCK_SIZE];
    if(!apfs_read(&ctx,0,sizeof(header),header,errp))return NULL;
    if(ldl_le_p(header+offsetof(ApfsNxSuperblock,magic))==APFS_NX_MAGIC)
        return apfs_open(&ctx,APFS_VOL_ROLE_ANY,path,errp);
    /* Stock OS DMG uses 512-byte GPT LBAs, restored NAND uses 4096. */
    GptHeader *gpt=(GptHeader *)(header+512);
    if(!memcmp(gpt->signature,"EFI PART",8)) {
        uint32_t count=le32_to_cpu(gpt->num_entries),stride=le32_to_cpu(gpt->entry_size);
        uint64_t table=le64_to_cpu(gpt->entries_lba)*512;
        if(!count || count>4096 || stride!=sizeof(GptEntry)){error_setg(errp,"Unsupported stock GPT bounds");return NULL;}
        for(uint32_t i=0;i<count;i++) {
            GptEntry entry;
            if(!apfs_read(&ctx,table+(uint64_t)i*stride,sizeof(entry),&entry,errp))return NULL;
            if(!memcmp(entry.type_guid,gpt_apfs_type_guid,16)) {
                ctx.dev.base=le64_to_cpu(entry.first_lba)*512;
                return apfs_open(&ctx,APFS_VOL_ROLE_ANY,path,errp);
            }
        }
        error_setg(errp,"No APFS partition in stock GPT");return NULL;
    }
    return apfs_open_file(blk,APFS_VOL_ROLE_SYSTEM,path,errp);
}
'''
adapted=out/'apfs-reader.c';adapted.write_text(source.read_text()+extra)
template=recipe['compile_template'];template=template[:template.index('-MD')]
for src,obj in [(adapted,out/'apfs-reader.o'),(Path(__file__).with_name('read-system-file.c').resolve(),out/'read.o')]:
    subprocess.run(template+['-I'+str(lab/'src/Inferno-current/include'),'-I'+str(lab/'src/Inferno-current'),'-c',str(src),'-o',str(obj)],cwd=build,check=True)
link=[str(out/'apfs-reader.o') if v.endswith('/fs-patcher-legacyblock/apfs.c.o') else v for v in recipe['link']
    if not v.endswith(('disk.c.o','main.cpp.o'))]
link[3]=str(out/'read-system-file');link.insert(4,str(out/'read.o'))
subprocess.run(link,cwd=build,check=True)
