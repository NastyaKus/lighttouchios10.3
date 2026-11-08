#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from system_results import loader_result,environment_result,backboardd_result

EXIT='*** EMBER-LOADER: child_exit=0\n'
NATIVE='*** EMBER-LOADER: PASS dependency=loaded native_arm64e_abi=0 PAC_IA0_verified\n'
NOOP='*** EMBER-LOADER: PASS dependency=loaded versioned_arm64e_loaded PAC_enforcement=unavailable\n'
HEADER='*** EMBER-LOADER: image_cpusubtype=0x80000002 signature_changed=1\n'
NEGATIVE='*** EMBER-LOADER: corrupted_PAC_rejected signal=11\n'
class EvidenceTest(unittest.TestCase):
    def test_backboardd_constructor_with_failed_main_cannot_become_ui_success(self):
        marker='*** EMBER-BB-LOADER: native_dependency_loaded program=backboardd dylib_cpusubtype=0x80000002 pid=45 uid=0 forwarding_only=1\n'
        self.assertIsNone(backboardd_result(marker))
        r=backboardd_result(marker+'*** EMBER-LOADER: child_signal=6 wait_status=0x6\n*** EMBER-LOADER: FAIL child_exit=134\n')
        self.assertEqual(r['state'],'backboardd_dependency_observed');self.assertTrue(r['backboardd_dependency_loaded'])
        self.assertEqual(r['backboardd_test_exit'],134);self.assertEqual(r['backboardd_test_signal'],6)
        self.assertFalse(r['system_gpu_verified']);self.assertFalse(r['backboardd_execution_verified'])
    def test_no_native_pac_claim_without_negative_and_changed_signature(self):
        for proof in [HEADER,NEGATIVE,HEADER.replace('=1','=0')+NEGATIVE,'']:
            with self.subTest(proof=proof),self.assertRaises(ValueError):loader_result(NATIVE+proof+EXIT)
        self.assertIsNone(loader_result(NATIVE+HEADER+NEGATIVE))
        result=loader_result(NATIVE+HEADER+NEGATIVE+EXIT)
        self.assertTrue(result['pointer_auth_ia_zero_verified']);self.assertFalse(result['system_gpu_verified'])
        self.assertFalse(result['backboardd_execution_verified'])
    def test_noop_is_loaded_header_only_never_pac_enforcement(self):
        result=loader_result(NOOP+HEADER.replace('=1','=0')+EXIT)
        self.assertTrue(result['versioned_arm64e_loaded']);self.assertFalse(result['pointer_auth_ia_zero_verified'])
        self.assertFalse(result['native_arm64e_abi_verified'])
        with self.assertRaises(ValueError):loader_result(NOOP+HEADER+EXIT)
    def test_arm64_positive_opcode_control_cannot_become_native_abi_proof(self):
        r=loader_result('*** EMBER-LOADER: PASS dependency=loaded PAC_instruction_roundtrip=int_float_context\n'+EXIT)
        self.assertFalse(r['versioned_arm64e_loaded']);self.assertFalse(r['pointer_auth_ia_zero_verified'])
    def test_environment_distinguishes_class_object_api_cache_and_busy_cleanup(self):
        text='*** EMBER-ENV: done frameworks=6/7 system_gpu_verified=0\n'+EXIT
        self.assertIsNone(environment_result(text.replace(EXIT,'')))
        r=environment_result(text+'*** EMBER-ENV: native_EAGLContext=present\n*** EMBER-ENV: native_init_API3=object\n*** EMBER-STOCK: unmount=-1 errno=16\n')
        self.assertTrue(r['native_eagl_api3_object']);self.assertFalse(r['native_eagl_api3_verified'])
        self.assertFalse(r['stock_system_cache_verified']);self.assertFalse(r['stock_system_unmount_verified'])
        self.assertEqual(r['stock_system_unmount_errno'],16)
        r=environment_result(text+'*** EMBER-ENV: runtime_cache_uuid=11ad39c9e3c93067a3377287744fdcb5\n*** EMBER-ENV: native_API=3 macro=present\n*** EMBER-STOCK: unmount=0 errno=0\n')
        self.assertTrue(r['stock_system_cache_verified']);self.assertTrue(r['native_eagl_api3_verified'])
        self.assertTrue(r['stock_system_unmount_verified']);self.assertFalse(r['system_gpu_verified'])
if __name__=='__main__':unittest.main(verbosity=2)
