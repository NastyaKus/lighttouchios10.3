#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot a caller-provided patch ramdisk against NEW sparse scratch disks.

No IPSW download, real OS disk, erase restore, or USB accept. The five Apple
inputs must already belong to the caller. Outputs remain in the new --work
directory; do not publish those private input copies or the ramdisk itself.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time
from system_results import loader_result,environment_result,backboardd_result,cleanup_result,exit_reason_result

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--qemu',type=Path,required=True)
p.add_argument('--inputs',type=Path,required=True,help='flat input files or a normal Inferno kit root')
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--ramdisk',type=Path,required=True)
p.add_argument('--memory-gb',type=int,choices=[2,3,4],default=2,help='Scratch guest RAM, default 2 GiB; never changes installed-kit settings')
p.add_argument('--expect-native-unmount',action='store_true',help='Also require real guest tmpfs mount/unmount success marker (synthetic daemon probe only)')
p.add_argument('--expect-daemon-self-test',action='store_true',help='Use ONLY with tools/guest-daemon-test/probe.c injected instead of the normal daemon; waits for two synthetic cycles')
p.add_argument('--expect-system-loader',action='store_true',help='Require the own Mach-O dependency/PAC loader fixture, not system GPU proof')
p.add_argument('--expect-system-env',action='store_true',help='Collect native framework/EAGL availability only, never a GPU success claim')
p.add_argument('--expect-backboardd-loader',action='store_true',help='Require native backboardd dependency constructor in disposable COW System; not a running UI')
p.add_argument('--readonly-system-socket',type=Path,help='Optional local NBD export of caller-owned stock System, read-only scratch environment modes only')
p.add_argument('--readonly-system-stub',type=Path,help='Linux file/NBD descriptor for a core without native NBD; requires matching LD_PRELOAD adapter, never a real image')
p.add_argument('--scratch-system-overlay',action='store_true',help='Create a bounded disposable QCOW2 overlay of the read-only stock source for a chroot-only runtime probe')
p.add_argument('--qemu-img',type=Path,help='Caller-provided qemu-img, required only for --scratch-system-overlay')
p.add_argument('--gpu-host',type=Path,help='Experimental GPU command host; requires gpu-bridge/guest-probe injected, adds a NEW diagnostic namespace')
p.add_argument('--gpu-gles-api',action='store_true',help='Expect the standard GLES-call probe (requires --gpu-host)')
p.add_argument('--gpu-eagl',action='store_true',help='Also require verified offscreen EAGL selector adapter')
p.add_argument('--gpu-present',action='store_true',help='Require the headless GPU presentation probe, not Android window proof')
p.add_argument('--gpu-broker',action='store_true',help='Require mobile client, raw-device denials and root broker completion')
p.add_argument('--work',type=Path,required=True,help='NEW scratch directory, never the kit directory')
a=p.parse_args();work=a.work.resolve();inputs=a.inputs.resolve()
if a.gpu_gles_api and not a.gpu_host:p.error('--gpu-gles-api requires --gpu-host')
if a.gpu_eagl and not a.gpu_gles_api:p.error('--gpu-eagl requires --gpu-gles-api')
if a.gpu_present and not a.gpu_eagl:p.error('--gpu-present requires --gpu-eagl')
if a.gpu_broker and not a.gpu_present:p.error('--gpu-broker requires --gpu-present')
if a.gpu_host and (not a.gpu_host.is_file() or a.expect_daemon_self_test or a.expect_native_unmount):
    p.error('--gpu-host requires an executable and cannot be combined with daemon test modes')
if a.expect_system_loader and (a.gpu_host or a.expect_daemon_self_test or a.expect_native_unmount):p.error('Loader fixture is a separate test mode')
if a.expect_system_env and (a.expect_system_loader or a.gpu_host or a.expect_daemon_self_test or a.expect_native_unmount):p.error('Native environment fixture is a separate test mode')
if a.expect_backboardd_loader and (a.expect_system_env or a.expect_system_loader or a.gpu_host or a.expect_daemon_self_test or a.expect_native_unmount or not a.scratch_system_overlay):p.error('Backboardd constructor fixture requires its own disposable COW System mode')
if a.readonly_system_socket:
    if not (a.expect_system_env or a.expect_system_loader or a.expect_backboardd_loader) or not a.readonly_system_socket.is_socket():p.error('Read-only System requires a local NBD socket and a system environment/loader probe')
    if ',' in str(a.readonly_system_socket) or len(str(a.readonly_system_socket.resolve()).encode())>=104:p.error('Invalid stock System socket path')
if a.readonly_system_stub:
    if not a.readonly_system_socket or not a.readonly_system_stub.is_file() or a.readonly_system_stub.stat().st_size!=7695611392:p.error('Stock descriptor requires the exact decoded System length and its NBD socket')
    if ',' in str(a.readonly_system_stub):p.error('Invalid stock descriptor path')
if a.scratch_system_overlay and (not a.readonly_system_socket or not a.qemu_img or not a.qemu_img.is_file() or not (a.expect_system_env or a.expect_system_loader or a.expect_backboardd_loader)):p.error('Scratch System overlay requires qemu-img and an isolated native environment/loader mode')
if work.exists() or work.is_relative_to(inputs) or inputs.is_relative_to(work):
    p.error('--work must be a new directory outside --inputs')
if not all(x.is_file() for x in [a.qemu,a.kernel,a.ramdisk]):p.error('Missing QEMU, kernel or ramdisk')
sources={
    'AppleSEPROM-Cebu-B1':'AppleSEPROM-Cebu-B1',
    'root_ticket.der':'InfernoData/root_ticket.der',
    'sep-firmware.n104.RELEASE.new.img4':'InfernoData/sep-firmware.n104.RELEASE.new.img4',
    'DeviceTree.n104ap.im4p':'InfernoData/Restore/Firmware/all_flash/DeviceTree.n104ap.im4p',
    '038-44135-124.dmg.trustcache':'InfernoData/Restore/Firmware/038-44135-124.dmg.trustcache',
}
for name,path in sources.items():
    source=inputs/path if (inputs/path).is_file() else inputs/name
    if not source.is_file():p.error('Missing input '+name)
    sources[name]=source
if len(str(work/'qmp.sock').encode())>=104:p.error('Use a shorter --work path for Unix sockets')
work.mkdir(parents=True)
for name,source in sources.items():shutil.copyfile(source,work/name)
sizes={'root':32<<30,'firmware':8<<20,'syscfg':128<<10,'ctrl_bits':8<<10,'nvram':8<<10,'effaceable':4096,'panic_log':1<<20,'sep_nvram':64<<10,'sep_ssc':128<<10}
for name,size in sizes.items():
    with (work/name).open('wb') as f:f.truncate(size)
with (work/'root').open('r+b') as f:
    f.write(b'EMBER-NONDESTRUCTIVE-NAND-PROBE'.ljust(65536,b'\x5a'))
    f.seek((32<<30)-65536);f.write(b'END-OF-SPARSE-SCRATCH-DISK'.ljust(65536,b'\xa5'))
def fingerprint():
    with (work/'root').open('rb') as f:
        b=f.read(65536);f.seek((32<<30)-65536);b+=f.read(65536)
    return hashlib.sha256(b).hexdigest()
before=fingerprint();usb=work/'usb.sock';qmp=work/'qmp.sock'
listener=socket.socket(socket.AF_UNIX);listener.bind(str(usb));listener.listen(1) # Never accept.
machine=f't8030,usb-uplink-type=inferno,usb-uplink-addr=unix:{usb},trustcache={work}/038-44135-124.dmg.trustcache,ticket={work}/root_ticket.der,sep-fw={work}/sep-firmware.n104.RELEASE.new.img4,sep-rom={work}/AppleSEPROM-Cebu-B1,kaslr-off=true,boot-mode=enter_recovery,disp-width=414,disp-height=896,disp-scale=1'
console=work/'console.log';console.write_text('')
cmd=[str(a.qemu.resolve()),'-accel','tcg,thread=multi,tb-size=64','-M',machine,'-kernel',str(a.kernel.resolve()),'-dtb',str(work/'DeviceTree.n104ap.im4p'),'-initrd',str(a.ramdisk.resolve()),'-append','tlto_us=-1 agm-genuine=1 agm-authentic=1 agm-trusted=1 serial=3 wdt=-1 launchd_unsecure_cache=1 -vm_compressor_wk_sw','-smp','4','-m',f'{a.memory_gb}G','-display','none','-serial','file:'+str(console),'-qmp',f'unix:{qmp},server=on,wait=off','-monitor','none','-global','driver=apple-smc,property=hold-reset,value=on']
for name in ['sep_nvram','sep_ssc']:cmd+=['-drive',f'file={work/name},if=pflash,format=raw']
for nsid,(name,nt) in enumerate([('root',1),('firmware',2),('syscfg',3),('ctrl_bits',4),('nvram',5),('effaceable',6),('panic_log',8)],1):
    cmd+=['-drive',f'file={work/name},format=raw,if=none,id={name}','-device',f'{"apple-nvram" if name=="nvram" else "nvme-ns"},drive={name},bus=nvme-bus.0,nsid={nsid},nstype={nt},logical_block_size=4096,physical_block_size=4096'+(',id=nvram' if name=='nvram' else '')]
gpu_process=None;gpu_output=None
system_overlay=None
if a.readonly_system_socket:
    stock_source=str(a.readonly_system_stub.resolve()) if a.readonly_system_stub else f'nbd+unix:///?socket={a.readonly_system_socket.resolve()}'
    stock_format='raw';stock_readonly='on'
    if a.scratch_system_overlay:
        system_overlay=work/'system-overlay.qcow2'
        subprocess.run([str(a.qemu_img.resolve()),'create','-f','qcow2','-F','raw','-b',stock_source,str(system_overlay)],check=True,stdout=subprocess.DEVNULL)
        stock_source=str(system_overlay);stock_format='qcow2';stock_readonly='off'
    cmd+=['-drive',f'file={stock_source},format={stock_format},if=none,id=stock-system,readonly={stock_readonly}',
        '-device','nvme-ns,drive=stock-system,bus=nvme-bus.0,nsid=8,nstype=2,logical_block_size=512,physical_block_size=512']
if a.gpu_host:
    gpu_output=(work/'gpu-host.log').open('w')
    gpu_process=subprocess.Popen([str(a.gpu_host.resolve()),str(work/'gpu-channel')],stdout=gpu_output,stderr=subprocess.STDOUT)
    deadline=time.monotonic()+15
    while not (work/'gpu-channel').is_file() and time.monotonic()<deadline and gpu_process.poll() is None:
        time.sleep(.05)
    if not (work/'gpu-channel').is_file():
        if gpu_process.poll() is None:gpu_process.terminate();gpu_process.wait(timeout=5)
        gpu_output.close();p.error('GPU command host did not create its scratch channel')
    cmd+=['-drive',f'file={work}/gpu-channel,format=raw,if=none,id=egpu,cache=none',
          '-device','nvme-ns,drive=egpu,bus=nvme-bus.0,nsid=8,nstype=2,logical_block_size=4096,physical_block_size=4096']
(work/'command.json').write_text(json.dumps(cmd,indent=2));start=time.monotonic()
result={'engine_source':'b76fa575831d94a80db169ffb3c76d9094a648bd','guest_memory':f'{a.memory_gb}G','guest_os_image_present':False,'start_restore_sent':False,'usb_accepted':False,'state':'running'}
if a.readonly_system_socket:result.update(stock_system_attached_readonly=not a.scratch_system_overlay,
    stock_system_backing_readonly=True,scratch_system_overlay=bool(a.scratch_system_overlay),guest_os_image_present=True,stock_system_boot_verified=False)
output=(work/'engine.log').open('w');guest=subprocess.Popen(cmd,stdout=output,stderr=subprocess.STDOUT)
try:
    ready=None;last=-20;gpu_done_at=None
    while time.monotonic()-start<180:
        text=console.read_text(errors='replace');elapsed=time.monotonic()-start
        if elapsed-last>15:
            print(json.dumps({'elapsed_seconds':round(elapsed),'console_bytes':console.stat().st_size,'nvme_mode_0':bool(re.search(r'Setting modesel to 0\b',text)),'patcher_waiting':'PATCHER: waiting for the restore' in text}),flush=True);last=elapsed
        if guest.poll() is not None:raise RuntimeError('QEMU exited '+str(guest.returncode))
        if 'panic(cpu' in text:raise RuntimeError('Guest kernel panic')
        if (work/'root').stat().st_blocks*512>16<<20:raise RuntimeError('Unexpected scratch root writes >16 MiB')
        if system_overlay and system_overlay.stat().st_blocks*512>128<<20:raise RuntimeError('Scratch System overlay exceeded its 128 MiB write budget')
        if a.expect_backboardd_loader:
            if '*** EMBER-STOCK: FAIL' in text or '*** EMBER-BB-LOADER: FAIL' in text:raise RuntimeError('Backboardd dependency environment/constructor failed; see console.log')
            evidence=backboardd_result(text)
            if evidence:result.update(evidence);break
            if '*** EMBER-LOADER: FAIL' in text:raise RuntimeError('Backboardd exited without the dependency constructor; see console.log')
            time.sleep(.5);continue
        if a.expect_system_env:
            if '*** EMBER-STOCK: FAIL' in text:raise RuntimeError('Read-only stock environment setup failed; see console.log')
            if '*** EMBER-LOADER: FAIL' in text:raise RuntimeError('Native environment probe failed; see console.log')
            evidence=environment_result(text)
            if evidence:result.update(evidence);break
            time.sleep(.5);continue
        if a.expect_system_loader:
            if '*** EMBER-LOADER: FAIL' in text:raise RuntimeError('Executable loader probe failed; see console.log')
            evidence=loader_result(text)
            if evidence:result.update(evidence);break
            time.sleep(.5);continue
        if a.gpu_host:
            if any(s in text for s in ['*** EGPU: FAIL','*** EGPU-GLES2: FAIL','*** EGPU-EAGL: FAIL','*** EGPU-PRESENT: FAIL','*** EGPU-BROKER: FAIL','*** EGPU-BROKER-CLIENT: FAIL']):raise RuntimeError('Guest GPU probe failed; see console.log')
            passed=('*** EGPU-GLES2: PASS checks=' in text) if a.gpu_gles_api else ('*** EGPU: PASS guest commands -> host GL -> verified red triangle / blue corner' in text)
            if a.gpu_broker:passed=passed and '*** EGPU-BROKER: PASS uid=501 gid=501' in text
            if passed:
                gpu_process.wait(timeout=5)
                if gpu_process.returncode!=0:raise RuntimeError('GPU command host exited nonzero')
                host_result=re.search(r'RESULT success=1 commands=(\d+) draws=(\d+) readback_pixels=(\d+)',(work/'gpu-host.log').read_text(errors='replace'))
                if not host_result:raise RuntimeError('GPU host did not confirm successful completion')
                if not a.gpu_gles_api and tuple(map(int,host_result.groups()))!=(7,2,2):
                    raise RuntimeError('GPU host command/readback counters differ from expected probe')
                if a.gpu_gles_api:
                    api_result=re.search(r'API_RESULT calls=(\d+) batches=(\d+) draws=(\d+) shader_compiles=(\d+)',(work/'gpu-host.log').read_text(errors='replace'))
                    if not api_result or any(v<minimum for v,minimum in zip(map(int,api_result.groups()),[80,10,4,5])):
                        raise RuntimeError('GPU GLES API counters incomplete')
                    guest_result=re.search(r'\*\*\* EGPU-GLES2: PASS checks=(\d+) packets=(\d+)',text)
                    commands,draws,pixels=map(int,host_result.groups())
                    if (not guest_result or int(guest_result.group(1))<40 or
                        commands!=int(guest_result.group(2))+1 or draws<4 or pixels<16384):
                        raise RuntimeError('GPU GLES guest/host counters or readback incomplete')
                    if a.gpu_eagl and '*** EGPU-EAGL: PASS API=2 current/detach/reattach;' not in text:
                        raise RuntimeError('EAGL selector adapter was not verified')
                    if a.gpu_present:
                        present=re.search(r'PRESENT_RESULT frames=(\d+) window_frames=(\d+) sample_pixels=(\d+) cpu_upload_bytes=(\d+)',(work/'gpu-host.log').read_text(errors='replace'))
                        if (not present or tuple(map(int,present.groups()))!=(60,0,240,0) or
                            '*** EGPU-PRESENT: PASS frames=60 window_frames=0 checks=191' not in text):
                            raise RuntimeError('Headless presentation counters/guest evidence incomplete')
                    if a.gpu_broker:
                        broker=re.search(r'\*\*\* EGPU-BROKER: PASS uid=501 gid=501 forwarded=(\d+) replayed=0 child_exit=0 cleanup=1(?:\r*\n|$)',text)
                        denied=re.search(r'\*\*\* EGPU-BROKER-CLIENT: uid=501 gid=501 raw_read_denied=(1|13) raw_write_denied=(1|13)\b',text)
                        if not broker or not denied or int(broker.group(1))!=commands or '*** EGPU-BROKER: authenticated uid=501 gid=501' not in text:
                            raise RuntimeError('Broker isolation/counters/client exit evidence incomplete')
                services=[{'class':m[0],'count':int(m[1]),'status':int(m[2])}
                    for m in re.findall(r'EGPU: registry class=(\S+) count=(\d+) status=(-?\d+)',text)]
                surface=re.search(r'EGPU: IOSurface id=(\d+) width=(\d+) height=(\d+) stride=(\d+) lock=(-?\d+) unlock=(-?\d+) cpu_mapping_verified=(\d+)',text)
                result.update(state='gpu_commands_verified',guest_command_roundtrip=True,guest_readback_verified=True,
                    guest_registry=services,guest_graphics_driver_implemented=False,android_adreno_tested=False,
                    host_backend=re.findall(r'EGPU(?:-GLES2)?: backend (.*)',text),gpu_commands=int(host_result.group(1)))
                if a.gpu_gles_api:
                    result['gles_api']=dict(zip(['calls','batches','draws','shader_compiles'],map(int,api_result.groups())))
                    result['gles_api']['scope']='experimental GLES2 subset, no system-wide integration'
                    result['gles_api']['checks']=int(guest_result.group(1))
                    result['eagl_offscreen_adapter_verified']=a.gpu_eagl
                    if a.gpu_present:result['headless_presentation']={'frames':60,'window_frames':0,'sample_pixels':240,'cpu_upload_bytes':0,'android_surface_verified':False}
                    if a.gpu_broker:result['broker']={'uid':501,'gid':501,'raw_read_denied':True,'raw_write_denied':True,'forwarded':commands,'client_exit':0,'app_store_sandbox_verified':False}
                if surface:
                    result['guest_iosurface']=dict(zip(['id','width','height','stride','lock','unlock','cpu_mapping_verified'],map(int,surface.groups())))
                break
            if gpu_process.poll() is not None:
                if gpu_process.returncode!=0:raise RuntimeError('GPU command host exited before guest PASS')
                # A successful host exits after writing DESTROY's reply. The
                # guest still has to read it and flush its PASS to the console.
                if gpu_done_at is None:gpu_done_at=elapsed
                elif elapsed-gpu_done_at>5:raise RuntimeError('GPU host finished but guest PASS was not observed')
            time.sleep(.5);continue
        if a.expect_daemon_self_test and 'EMBER NATIVE UNMOUNT TEST FAILED' in text:raise RuntimeError('Native guest mount/unmount test failed')
        if a.expect_daemon_self_test and 'EMBER DAEMON SYNTHETIC GUEST RESULT' in text and 'EMBER DAEMON SYNTHETIC GUEST RESULT first=0 second=0' not in text:raise RuntimeError('Synthetic guest daemon failed')
        if re.search(r'Setting modesel to 0\b',text) and 'Skipped firmware update successfully' in text and 'PATCHER: waiting for the restore' in text:
            if ready is None:ready=elapsed
            if elapsed-ready>5 and (not a.expect_daemon_self_test or 'EMBER DAEMON SYNTHETIC GUEST RESULT first=0 second=0' in text) and (not a.expect_native_unmount or 'EMBER NATIVE UNMOUNT RESULT mount=0 unmount=0 errno=0 missing=-1 missing_errno=' in text):
                result.update(state='storage_initialized',nvme_mode_0=True,nvme_skip_success=True,patcher_started=True,storage_job_loaded='dev.embervm.storage-ready' in text,synthetic_daemon_cycles_verified=2 if a.expect_daemon_self_test else 0,native_tmpfs_unmount_verified='EMBER NATIVE UNMOUNT RESULT mount=0 unmount=0 errno=0 missing=-1 missing_errno=' in text);break
        time.sleep(.5)
    else:raise RuntimeError('Guest probe exceeded 180 seconds')
except Exception as error:result.update(state='failed',error=str(error))
finally:
    if guest.poll() is None:
        try:
            channel=socket.socket(socket.AF_UNIX);channel.settimeout(3);channel.connect(str(qmp));channel.recv(65536)
            channel.sendall(b'{"execute":"qmp_capabilities"}\r\n');channel.recv(65536);channel.sendall(b'{"execute":"quit"}\r\n');channel.close();guest.wait(timeout=8)
        except Exception:
            guest.terminate()
            try:guest.wait(timeout=5)
            except subprocess.TimeoutExpired:guest.kill();guest.wait()
    listener.close();output.close()
    if gpu_process:
        if gpu_process.poll() is None:gpu_process.terminate();gpu_process.wait(timeout=5)
        gpu_output.close()
    after=fingerprint()
    if a.readonly_system_socket:result.update(cleanup_result(console.read_text(errors='replace')))
    if a.expect_backboardd_loader:result.update(exit_reason_result(console.read_text(errors='replace')))
    result.update(elapsed_seconds=round(time.monotonic()-start),qemu_exit=guest.returncode,root_header_and_tail_sha256_before=before,root_header_and_tail_sha256_after=after,root_sentinel_preserved=before==after,root_physical_bytes=(work/'root').stat().st_blocks*512,filesystem_patches_verified=False,first_ios_boot_verified=False)
    if before!=after:result.update(state='failed',error='Scratch root sentinels changed')
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
if result['state']!=('backboardd_dependency_observed' if a.expect_backboardd_loader else 'system_environment_reported' if a.expect_system_env else 'own_loader_verified' if a.expect_system_loader else 'gpu_commands_verified' if a.gpu_host else 'storage_initialized'):raise SystemExit(1)
