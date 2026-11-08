#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""CLI-compatible JVM wrapper for patch-boot-probe; dedicated scratch files only."""
import base64
import os
from pathlib import Path
import subprocess
import sys

image=Path(sys.argv[1]).resolve()
build=Path(os.environ.get('EGPU_JNI_BUILD',Path(__file__).resolve().parents[2]/'build/gpu-jni')).resolve()
log=image.with_name(image.name+'.jni.log');result=image.with_name(image.name+'.jni-result')
if log.exists() or result.exists():raise SystemExit('JNI output paths must be new')
p=subprocess.run([str(Path(os.environ['JAVA_HOME'])/'bin/java'),'-Djava.library.path='+str(build),
    '-cp',str(build),'dev.embervm.android.GpuHostNative',str(image),str(log),str(result)] + sys.argv[2:],
    capture_output=True,timeout=190)
if log.is_file():print(log.read_text(errors='replace'),end='',flush=True)
if p.stdout:sys.stdout.buffer.write(p.stdout)
if p.stderr:sys.stderr.buffer.write(p.stderr)
if result.is_file():
    values=[base64.b64decode(line).decode() for line in result.read_text().splitlines()]
    assert len(values)==12 and values[0] in ('0','1')
    if values[0]=='1':
        assert all(int(v)>=0 for v in values[5:])
    else:print('JNI failure: '+values[4],flush=True)
else:raise SystemExit('JNI fixture produced no result')
raise SystemExit(p.returncode)
