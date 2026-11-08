#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real JVM/JNI/EGL tests on Linux, no Android lifecycle coverage."""
import argparse
import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--client',type=Path,required=True);p.add_argument('--present',action='store_true');p.add_argument('--broker',action='store_true');a=p.parse_args()
if a.broker and not a.present:p.error('--broker requires --present')
host=Path(__file__).with_name('host.py')
def result(image):return [base64.b64decode(line).decode() for line in image.with_name(image.name+'.jni-result').read_text().splitlines()]
with tempfile.TemporaryDirectory(prefix='ember-gpu-jni-') as td:
    root=Path(td);image=root/'channel'
    with (root/'host-output').open('w') as log:
        proc=subprocess.Popen([str(host),str(image)],stdout=log,stderr=subprocess.STDOUT)
        try:
            until=time.monotonic()+10
            while not image.exists() and proc.poll() is None and time.monotonic()<until:time.sleep(.01)
            executable=a.client.resolve()
            command=([str(executable.with_name('broker-launcher-linux')),'--image',str(image),'--socket',str(root/'broker.sock'),'--client',str(executable)]
                if a.broker else [str(executable),'--image',str(image)])
            client=subprocess.run(command,capture_output=True,text=True,timeout=60 if a.broker else 35)
            proc.wait(timeout=10)
            assert client.returncode==0 and proc.returncode==0,(client.stdout,(root/'host-output').read_text())
            v=result(image);assert v[0]=='1' and int(v[5])==(316 if a.present else 119) and int(v[8])==(1262 if a.present else 194) and int(v[11])==(7 if a.present else 5),v
            stats=json.loads(image.with_name(image.name+'.jni-result.stats').read_text())
            assert len(stats)==14 and stats[0]==(60 if a.present else 0) and stats[1]==0 and stats[12]==(240 if a.present else 0) and stats[13]==0
            if a.broker:assert f'EGPU-BROKER: PASS uid={os.geteuid()} gid={os.getegid()} forwarded=316 replayed=0 child_exit=0 cleanup=1' in client.stdout,client.stdout
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
    cancel=root/'cancel';start=time.monotonic()
    r=subprocess.run([str(host),str(cancel),'cancel'],capture_output=True,text=True,timeout=15)
    v=result(cancel);assert r.returncode==1 and v[0]=='0' and v[4]=='Cancelled' and time.monotonic()-start<10,v
    existing=root/'existing';existing.write_bytes(b'user sentinel: must not be overwritten')
    r=subprocess.run([str(host),str(existing)],capture_output=True,text=True,timeout=15)
    assert r.returncode==1 and existing.read_bytes()==b'user sentinel: must not be overwritten'
    assert 'must not exist' in result(existing)[4]
print('PASS JNI: real GLES client roundtrip, 12 result fields, 14 presentation counters, cancellation, existing-file refusal; broker='+str(a.broker))
