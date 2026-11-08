#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Same C command client on Linux; real iOS boot uses patch-boot-probe/run.py."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("--build",type=Path,required=True)
p.add_argument("--client",type=Path,help="Override command client executable")
p.add_argument("--gles-api",action="store_true",help="Verify the GLES API client rather than legacy probe")
p.add_argument("--present",action="store_true",help="Also require the 60-frame headless presentation test")
p.add_argument("--broker",action="store_true",help="Run a client through the authenticated Unix broker (Linux credentials fixture only)")
a=p.parse_args();build=a.build.resolve()
if a.broker and not(a.gles_api and a.present):p.error('--broker requires --gles-api --present')
client_executable=a.client.resolve() if a.client else build/("broker-client-linux" if a.broker else "gles-api-probe-linux" if a.gles_api else "guest-probe-linux")
env=os.environ.copy();env.update(EGL_PLATFORM="surfaceless",LIBGL_ALWAYS_SOFTWARE="1")
with tempfile.TemporaryDirectory(prefix="ember-egpu-") as td:
    root=Path(td);image=root/"channel";log=root/"host.log"
    def command(image):
        return ([str(build/'broker-launcher-linux'),'--image',str(image),'--socket',str(root/'broker.sock'),'--client',str(client_executable)]
            if a.broker else [str(client_executable),'--image',str(image)])
    with log.open("w") as output:
        host=subprocess.Popen([str(build/"host-probe"),str(image)],env=env,stdout=output,stderr=subprocess.STDOUT)
        try:
            deadline=time.monotonic()+10
            while not image.exists() and time.monotonic()<deadline and host.poll() is None:time.sleep(.01)
            client=subprocess.run(command(image),capture_output=True,text=True,timeout=60 if a.broker else 35)
            host.wait(timeout=10)
            assert client.returncode==0 and host.returncode==0,(client.stdout,log.read_text())
            if a.gles_api:
                assert "EGPU-GLES2: PASS checks=" in client.stdout
                assert "batched ten GL calls into one request" in client.stdout
                assert "API_RESULT" in log.read_text() and ("shader_compiles=7" if a.present else "shader_compiles=5") in log.read_text()
                if a.present:
                    assert "*** EGPU-PRESENT: PASS frames=60 window_frames=0" in client.stdout
                    assert "PRESENT_RESULT frames=60 window_frames=0 sample_pixels=240 cpu_upload_bytes=0" in log.read_text()
                if a.broker:
                    proof=re.search(r'EGPU-BROKER: PASS uid=(\d+) gid=(\d+) forwarded=316 replayed=0 child_exit=0 cleanup=1',client.stdout)
                    assert proof and tuple(map(int,proof.groups()))==(os.geteuid(),os.getegid()),client.stdout
                    assert 'Linux IPC fixture' in client.stdout and 'success=1 commands=316 ' in log.read_text()
            else:
                assert "PASS guest commands -> host GL -> verified red triangle / blue corner" in client.stdout
                assert "success=1 commands=7 draws=2 readback_pixels=2" in log.read_text()
            print(client.stdout.strip())
        finally:
            if host.poll() is None:host.terminate();host.wait(timeout=5)
    original=image.read_bytes()
    result=subprocess.run([str(build/"host-probe"),str(image)],env=env,capture_output=True,timeout=10)
    assert result.returncode!=0 and image.read_bytes()==original
    fake=root/"unmarked";fake.write_bytes(bytes(16384));before=fake.read_bytes()
    result=subprocess.run(command(fake),capture_output=True,timeout=10)
    assert result.returncode!=0 and fake.read_bytes()==before
    print("PASS file transport: roundtrip, existing-file refusal, unmarked-channel refusal")
