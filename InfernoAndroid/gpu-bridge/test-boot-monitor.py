#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic process ordering tests; these do not execute iOS or OpenGL."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

RUNNER=Path(__file__).resolve().parents[1]/"tools/patch-boot-probe/run.py"

class BootMonitorTest(unittest.TestCase):
    def run_fixture(self,host_status=0,guest_pass=True,gles=False,eagl=False,
                    eagl_marker=True,api_counters=(194,18,4,5),host_commands=None,
                    present=False,presentation_counters=(60,0,240,0),present_marker=True,
                    broker=False,broker_marker=True,broker_denied=True,broker_commands=316,broker_cleanup=1):
        with tempfile.TemporaryDirectory(prefix="egpu-monitor-") as td:
            root=Path(td);inputs=root/"inputs";inputs.mkdir()
            for name in ["AppleSEPROM-Cebu-B1","root_ticket.der","sep-firmware.n104.RELEASE.new.img4",
                         "DeviceTree.n104ap.im4p","038-44135-124.dmg.trustcache","kernel","ramdisk"]:
                (inputs/name).write_bytes(b"synthetic test fixture, not Apple firmware")
            guest=root/"fake-qemu";host=root/"fake-gpu"
            marker=('*** EGPU-GLES2: PASS checks=40 packets=118\n' if gles else
                    '*** EGPU: PASS guest commands -> host GL -> verified red triangle / blue corner\n')
            if eagl and eagl_marker:
                marker='*** EGPU-EAGL: PASS API=2 current/detach/reattach; TLS ownership verified\n'+marker
            if present:
                marker=marker.replace('packets=118','packets=315')
                if present_marker:marker='*** EGPU-PRESENT: PASS frames=60 window_frames=0 checks=191\n'+marker
            if broker:
                marker='*** EGPU-BROKER: authenticated uid=501 gid=501\n'+marker
                if broker_denied:marker='*** EGPU-BROKER-CLIENT: uid=501 gid=501 raw_read_denied=1 raw_write_denied=13\n'+marker
                if broker_marker:marker+=f'*** EGPU-BROKER: PASS uid=501 gid=501 forwarded={broker_commands} replayed=0 child_exit=0 cleanup={broker_cleanup}\n'
            guest.write_text("#!/usr/bin/env python3\nimport sys,time\nfrom pathlib import Path\n"
                "p=Path(sys.argv[sys.argv.index('-serial')+1][5:])\ntime.sleep(.8)\n"+
                (f"p.write_text({marker!r})\n" if guest_pass else "")+
                "time.sleep(30)\n")
            api_line='API_RESULT calls=%d batches=%d draws=%d shader_compiles=%d'%((1262,85,64,7) if present else api_counters)
            counts=(316,307,16391) if present else (119,7,16391) if gles else (7,2,2)
            present_line='PRESENT_RESULT frames=%d window_frames=%d sample_pixels=%d cpu_upload_bytes=%d'%presentation_counters
            if host_commands is not None:counts=(host_commands,*counts[1:])
            host_line='RESULT success=%d commands=%d draws=%d readback_pixels=%d'%(int(host_status==0),*counts)
            host.write_text("#!/usr/bin/env python3\nimport sys,time\nfrom pathlib import Path\n"
                "Path(sys.argv[1]).write_bytes(b'synthetic channel')\ntime.sleep(.05)\n"
                +(f"print({api_line!r},flush=True)\n" if gles else "")+
                (f"print({present_line!r},flush=True)\n" if present else "")+
                f"print({host_line!r},flush=True)\n"
                f"sys.exit({host_status})\n")
            guest.chmod(0o755);host.chmod(0o755)
            result=subprocess.run(["python3",str(RUNNER),"--qemu",str(guest),"--inputs",str(inputs),
                "--kernel",str(inputs/"kernel"),"--ramdisk",str(inputs/"ramdisk"),"--gpu-host",str(host),"--work",str(root/"boot")]
                +(['--gpu-gles-api'] if gles else [])+(['--gpu-eagl'] if eagl else [])+(['--gpu-present'] if present else [])+(['--gpu-broker'] if broker else []),
                capture_output=True,text=True,timeout=20)
            return result,json.loads((root/"boot/result.json").read_text())
    def test_host_exit_before_console_flush_is_not_false_failure(self):
        p,r=self.run_fixture()
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertEqual(r["state"],"gpu_commands_verified")
        self.assertTrue(r["root_sentinel_preserved"])
        self.assertFalse(r["first_ios_boot_verified"])
    def test_nonzero_host_never_becomes_success(self):
        p,r=self.run_fixture(host_status=1)
        self.assertNotEqual(p.returncode,0)
        self.assertEqual(r["state"],"failed")
    def test_zero_host_without_guest_pass_never_becomes_success(self):
        p,r=self.run_fixture(guest_pass=False)
        self.assertNotEqual(p.returncode,0)
        self.assertIn("PASS was not observed",r["error"])
    def test_eagl_success_requires_both_api_counters_and_selector_marker(self):
        p,r=self.run_fixture(gles=True,eagl=True)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertTrue(r['eagl_offscreen_adapter_verified'])
        self.assertEqual(r['gles_api']['shader_compiles'],5)
        self.assertFalse(r['guest_graphics_driver_implemented'])
        self.assertFalse(r['android_adreno_tested'])
    def test_eagl_mode_without_selector_marker_is_failure(self):
        p,r=self.run_fixture(gles=True,eagl=True,eagl_marker=False)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('EAGL selector adapter was not verified',r['error'])
    def test_api_pass_text_without_real_shader_and_draw_counters_is_failure(self):
        p,r=self.run_fixture(gles=True,api_counters=(191,18,0,0))
        self.assertNotEqual(p.returncode,0)
        self.assertIn('API counters incomplete',r['error'])
    def test_guest_packet_count_must_match_host_commands_including_destroy(self):
        p,r=self.run_fixture(gles=True,host_commands=118)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('guest/host counters',r['error'])
    def test_headless_presentation_remains_explicitly_without_android_surface_proof(self):
        p,r=self.run_fixture(gles=True,eagl=True,present=True)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertEqual(r['headless_presentation']['frames'],60)
        self.assertFalse(r['headless_presentation']['android_surface_verified'])
    def test_headless_counter_without_guest_present_marker_is_failure(self):
        p,r=self.run_fixture(gles=True,eagl=True,present=True,present_marker=False)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('Headless presentation counters',r['error'])
    def test_broker_success_requires_complete_isolation_and_exit_evidence(self):
        p,r=self.run_fixture(gles=True,eagl=True,present=True,broker=True)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertTrue(r['broker']['raw_read_denied']);self.assertTrue(r['broker']['raw_write_denied'])
        self.assertEqual(r['broker']['forwarded'],316);self.assertFalse(r['broker']['app_store_sandbox_verified'])
    def test_broker_host_completion_without_parent_pass_is_failure(self):
        p,r=self.run_fixture(gles=True,eagl=True,present=True,broker=True,broker_marker=False)
        self.assertNotEqual(p.returncode,0);self.assertIn('PASS was not observed',r['error'])
    def test_broker_without_raw_denial_or_cleanup_or_matching_counter_is_failure(self):
        for options in [dict(broker_denied=False),dict(broker_commands=315),dict(broker_cleanup=0)]:
            with self.subTest(options=options):
                p,r=self.run_fixture(gles=True,eagl=True,present=True,broker=True,**options)
                self.assertNotEqual(p.returncode,0);self.assertIn('Broker isolation',r['error'])

if __name__=="__main__":unittest.main()
