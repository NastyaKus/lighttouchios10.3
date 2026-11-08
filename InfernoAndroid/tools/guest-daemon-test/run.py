#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the real C daemon on Linux with compile-time sandbox paths/fake guest tools.
No caller disks/images are read. Each test owns a temporary directory.
"""
import json, os, pathlib, plistlib, subprocess, tempfile, unittest
PROJECT = pathlib.Path(__file__).resolve().parents[2]
PLAN=json.loads((PROJECT/'guest/plans/18A5351d-v031-software.json').read_text())['patches']
SOURCE = pathlib.Path(os.environ.get('EMBER_DAEMON_SOURCE',str(PROJECT/'guest/patcher-daemon.c')))
class DaemonTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ember-daemon-')
        self.root = pathlib.Path(self.temp.name)
        self.cache = self.root/'mnt9/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e'
        self.cache.parent.mkdir(parents=True)
        for name in ('dev', 'sbin', 'mnt8', 'usr/standalone/firmware/nfrestore/firmware/jcop-prod'):
            (self.root/name).mkdir(parents=True,exist_ok=True)
        for name in ('disk0s1s1','disk0s1s2'):
            (self.root/'dev'/name).touch()
        # Backup is intentionally created FIRST, reproducing readdir ordering.
        self.backup = pathlib.Path(str(self.cache)+'.InfernoOriginalBytes')
        self.backup.write_bytes(b'8143964: 1f 20 03 d5\n')
        pathlib.Path(str(self.cache)+'.map').write_text('this is not a cache')
        with self.cache.open('wb') as file:
            file.write(b'dyld_v1  arm64e\0\0'); file.truncate(1957*1024*1024)
        self.tool('sbin/mount_apfs', 'import sys\nsys.exit(0)\n')
        self.patcher = 'usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-11.04-012.2-P.bin'
        self.tool(self.patcher, 'import pathlib,sys\np=pathlib.Path(sys.argv[1])\nassert p.name=="dyld_shared_cache_arm64e"\nassert pathlib.Path(str(p)+".InfernoOriginalBytes").read_bytes()==b"8143964: 1f 20 03 d5\\n"\nprint("Changes applied successfully.")\n'+'with p.open("r+b") as f:\n for row in '+repr(PLAN)+':\n  f.seek(row["offset"]);f.write(bytes(row["bytes"]))\n')
        self.launchd=self.root/'mnt9/System/Library/xpc/launchd.plist'
        self.launchd.parent.mkdir(parents=True)
        self.labels=['com.apple.voicemail.vmd','com.apple.CommCenter','com.apple.CommCenterMobileHelper','com.apple.CommCenterRootHelper','com.apple.locationd']
        jobs={s:{'Label':s,'ProgramArguments':['/original',s],'Disabled':False} for s in self.labels+['com.apple.SpringBoard','example.other']}
        self.original_plist=plistlib.dumps({'LaunchDaemons':jobs,'Other':[False,False,True]},fmt=plistlib.FMT_BINARY)
        self.launchd.write_bytes(self.original_plist)
        self.bin = self.root/'daemon'
        subprocess.run([os.environ.get('CC','cc'),'-O2','-Wall','-Wextra','-Werror',f'-DEMBER_TEST_ROOT="{self.root}"',str(SOURCE),str(PROJECT/'guest/launchd-cache.c'),str(PROJECT/'tools/guest-daemon-test/unmount-wrap.c'),'-Wl,--wrap=umount2','-Wl,--wrap=sync','-o',str(self.bin)],check=True)
    def tearDown(self): self.temp.cleanup()
    def tool(self,path,body):
        f=self.root/path; f.write_text('#!/usr/bin/python3\n'+body); f.chmod(0o755)
    def run_daemon(self,unmount_failure=''):
        env=os.environ.copy();env['EMBER_TEST_UNMOUNT_FAILURE']=unmount_failure
        return subprocess.run([self.bin],capture_output=True,text=True,timeout=10,env=env)
    def test_backup_first_and_repeat_select_only_exact_cache(self):
        for _ in range(2):
            r=self.run_daemon(); self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn('patch plan verified 18A5351d-v031-software all=45',r.stdout)
            self.assertNotIn('patching '+str(self.backup),r.stdout)
            self.assertIn('unmounting '+str(self.root/'mnt9'),r.stdout)
            self.assertNotIn('mounting '+str(self.root/'dev/disk0s1s2'),r.stdout)
            self.assertLess(r.stdout.index('five services switched off'),r.stdout.index('unmount '+str(self.root/'mnt9')+' says 0'))
            self.assertLess(r.stdout.index('unmount '+str(self.root/'mnt9')+' says 0'),r.stdout.index('done, the machine can be reset now'))
            p=plistlib.loads(self.launchd.read_bytes())
            for name in self.labels:self.assertIs(p['LaunchDaemons'][name]['Disabled'],True)
            self.assertIs(p['LaunchDaemons']['example.other']['Disabled'],False)
            self.assertEqual(self.launchd.with_name('launchd.plist.ember-original').read_bytes(),self.original_plist)
        self.assertEqual(self.backup.read_bytes(),b'8143964: 1f 20 03 d5\n')
    def apply_plan(self):
        with self.cache.open('r+b') as f:
            for row in PLAN: f.seek(row['offset']);f.write(bytes(row['bytes']))
    def test_complete_plan_without_backup_is_reused_without_running_patcher(self):
        self.apply_plan();self.backup.unlink()
        self.tool(self.patcher,'import sys\nsys.exit(99)\n')
        before=self.cache.stat().st_mtime_ns
        r=self.run_daemon();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('existing complete patch set reused; cache not rewritten',r.stdout)
        self.assertNotIn('the patcher exited with',r.stdout);self.assertEqual(before,self.cache.stat().st_mtime_ns)
    def test_partial_plan_and_fake_exit_zero_never_become_success(self):
        self.apply_plan()
        with self.cache.open('r+b') as f: f.seek(PLAN[0]['offset']);f.write(b'X')
        self.tool(self.patcher,'import sys\nsys.exit(0)\n')
        r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertIn('matched=44 total=45',r.stdout);self.assertNotIn('five services',r.stdout)
        self.assertIn('expected patch plan is incomplete',r.stdout)
    def test_truncated_cache_cannot_verify_out_of_bounds_plan_sites(self):
        self.cache.write_bytes(b'dyld_v1  arm64e\0\0')
        with self.cache.open('r+b') as f:f.truncate(17*1024*1024)
        self.tool(self.patcher,'import sys\nsys.exit(1)\n')
        r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertIn('matched=0 total=45',r.stdout);self.assertNotIn('patch plan verified',r.stdout)
    def test_failure_cleanup_marker_precedes_error_and_unmount_has_finished(self):
        self.tool(self.patcher,'import sys\nsys.exit(1)\n')
        r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertLess(r.stdout.index('unmount '+str(self.root/'mnt9')+' says 0'),r.stdout.index('cleanup complete'))
        self.assertLess(r.stdout.index('cleanup complete'),r.stdout.index('ERROR:'))
    def test_no_umount_executable_required(self):
        self.assertFalse((self.root/'sbin/umount').exists())
        r=self.run_daemon();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('unmount '+str(self.root/'mnt9')+' says 0 errno=0',r.stdout)
        self.assertIn('done, the machine can be reset now',r.stdout)
    def test_backup_only_fails_without_running_patcher(self):
        self.cache.unlink();r=self.run_daemon()
        self.assertNotEqual(r.returncode,0);self.assertIn('ERROR: expected arm64e',r.stdout)
        self.assertNotIn('the patcher exited with',r.stdout);self.assertNotIn('done, the machine',r.stdout)
    def test_invalid_magic_or_small_cache_is_rejected(self):
        with self.cache.open('r+b') as f: f.write(b'not a dyld cache')
        r=self.run_daemon();self.assertNotEqual(r.returncode,0);self.assertIn('ERROR:',r.stdout)
        self.cache.write_bytes(b'dyld_v1  arm64e\0\0');r=self.run_daemon()
        self.assertNotEqual(r.returncode,0);self.assertNotIn('the patcher exited with',r.stdout)
    def test_patcher_nonzero_does_not_write_overrides_or_done(self):
        self.tool(self.patcher,'import sys\nprint("synthetic failure")\nsys.exit(1)\n')
        r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertIn('the patcher exited with 1',r.stdout);self.assertIn('synthetic failure',r.stdout)
        self.assertEqual(self.launchd.read_bytes(),self.original_plist)
        self.assertNotIn('done, the machine',r.stdout)
    def test_patcher_signal_is_not_success(self):
        self.tool(self.patcher,'import os,signal\nos.kill(os.getpid(),signal.SIGKILL)\n')
        r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertIn('the patcher exited with -9',r.stdout);self.assertNotIn('five services',r.stdout)
    def test_unmount_failure_after_service_patch_never_finishes(self):
        r=self.run_daemon('all');self.assertNotEqual(r.returncode,0)
        self.assertIn('the patcher exited with 0',r.stdout)
        self.assertIn('says -1 errno=16',r.stdout)
        self.assertNotIn('done, the machine',r.stdout)
    def test_atomic_cache_write_failure_keeps_original_and_never_emits_done(self):
        self.launchd.with_name('launchd.plist.ember-part').mkdir()
        r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertEqual(self.launchd.read_bytes(),self.original_plist)
        self.assertNotIn('five services switched off',r.stdout);self.assertNotIn('done, the machine',r.stdout)
        self.assertIn('cleanup complete',r.stdout)
    def test_missing_service_cache_is_not_success(self):
        self.launchd.unlink();r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertNotIn('five services switched off',r.stdout);self.assertIn('System launchd service cache update failed',r.stdout)
    def test_missing_target_rejects_before_cache_write(self):
        p=plistlib.loads(self.original_plist);del p['LaunchDaemons'][self.labels[0]]
        data=plistlib.dumps(p,fmt=plistlib.FMT_BINARY);self.launchd.write_bytes(data)
        r=self.run_daemon();self.assertNotEqual(r.returncode,0)
        self.assertEqual(self.launchd.read_bytes(),data);self.assertNotIn('five services switched off',r.stdout)
    def test_no_data_node_or_mount_is_required(self):
        (self.root/'dev/disk0s1s2').unlink()
        r=self.run_daemon();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertNotIn('mounting '+str(self.root/'dev/disk0s1s2'),r.stdout)
if __name__=='__main__': unittest.main(verbosity=2)
