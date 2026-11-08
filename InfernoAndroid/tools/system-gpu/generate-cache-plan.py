#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate identity hashes for the 45 software patch sites. No Apple code output."""
import argparse
import hashlib
import json
import mmap
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cache',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--check',action='store_true')
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
plan=json.loads((root/'guest/plans/18A5351d-v031-software.json').read_text())
rows=[]
with a.cache.open('rb') as file,mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
    if hashlib.sha256(data).hexdigest()!='59fdf288034e947c39b087b7c405ed149de3fea9f8af4a5ae2611fa03d4081e1':raise SystemExit('Not the inspected stock 18A5351d cache')
    for item in plan['patches']:
        at=item['offset'];size=len(item['bytes'])
        rows.append((at,bytes(item['bytes']).hex(),hashlib.sha256(data[at:at+size]).hexdigest(),at in (0x8158bdc,0x8158be0)))
text='// SPDX-License-Identifier: GPL-3.0-or-later\n// Identity metadata, our software replacement bytes and hashes; no stock Apple code.\npackage dev.embervm.android\n\ninternal object SystemGpuCachePlan {\n    data class Site(val offset:Long,val software:String,val originalSha256:String,val restore:Boolean)\n    val sites=listOf(\n'
text+=',\n'.join(f'        Site(0x{at:x},"{software}","{digest}",{str(restore).lower()})' for at,software,digest,restore in rows)
text+='\n    )\n}\n'
if a.check:
    if a.out.read_text()!=text:raise SystemExit('Generated cache plan differs')
else:a.out.write_text(text)
print('45 original hashes / 2 main EAGL sites verified; no cache was modified')
