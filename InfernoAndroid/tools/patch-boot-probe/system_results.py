# SPDX-License-Identifier: GPL-3.0-or-later
"""Parse narrow evidence from our own processes; never infer iOS acceleration."""
import re

STOCK_CACHE_UUID='11ad39c9e3c93067a3377287744fdcb5'

def cleanup_result(text):
    match=re.search(r'\*\*\* EMBER-STOCK: unmount=(-?\d+) errno=(\d+)',text)
    return {'stock_system_unmount_verified':bool(match and match[1]=='0' and match[2]=='0'),
        'stock_system_unmount_errno':int(match[2]) if match else None}

def exit_reason_result(text):
    reason=re.search(r'\*\*\* EMBER-BB-LOADER: exit_reason namespace=(\d+) code=(\d+) flags=0x([0-9a-f]+)',text)
    return dict(backboardd_exit_reason_namespace=int(reason[1]),backboardd_exit_reason_code=int(reason[2]),
        backboardd_exit_reason_flags=int(reason[3],16)) if reason else {}

def loader_result(text):
    if '*** EMBER-LOADER: child_exit=0' not in text:return None
    native='*** EMBER-LOADER: PASS dependency=loaded native_arm64e_abi=0 PAC_IA0_verified' in text
    noop='*** EMBER-LOADER: PASS dependency=loaded versioned_arm64e_loaded PAC_enforcement=unavailable' in text
    control='*** EMBER-LOADER: PASS dependency=loaded PAC_instruction_roundtrip=int_float_context' in text
    if not (native or noop or control):return None
    if noop and '*** EMBER-LOADER: image_cpusubtype=0x80000002 signature_changed=0' not in text:
        raise ValueError('Versioned arm64e NOOP result lacks its header/control evidence')
    if native and (not re.search(r'\*\*\* EMBER-LOADER: corrupted_PAC_rejected signal=(10|11)\b',text) or
        '*** EMBER-LOADER: image_cpusubtype=0x80000002 signature_changed=1' not in text):
        raise ValueError('Native arm64e claim lacks header, changed signature or negative PAC proof')
    return dict(state='own_loader_verified',own_test_executable=True,dependency_loaded=True,
        pac_instruction_roundtrip=True,pointer_auth_ia_zero_verified=native,native_arm64e_abi_verified=native,
        versioned_arm64e_loaded=native or noop,
        wire_crypto_verified='*** EMBER-LOADER: wire_crypto=verified SHA256_RFC4231_CRC' in text,
        backboardd_execution_verified=False,system_gpu_verified=False,**cleanup_result(text))

def environment_result(text):
    match=re.search(r'\*\*\* EMBER-ENV: done frameworks=(\d+)/7 system_gpu_verified=0',text)
    if not match or '*** EMBER-LOADER: child_exit=0' not in text:return None
    uuid=re.search(r'\*\*\* EMBER-ENV: runtime_cache_uuid=([0-9a-f]{32})\b',text)
    return dict(state='system_environment_reported',native_frameworks_available=int(match[1]),
        runtime_cache_uuid=uuid[1] if uuid else None,
        stock_system_cache_verified=bool(uuid and uuid[1]==STOCK_CACHE_UUID),
        native_eagl_class_present='*** EMBER-ENV: native_EAGLContext=present' in text,
        native_eagl_api3_object='*** EMBER-ENV: native_init_API3=object' in text,
        native_eagl_api3_verified='*** EMBER-ENV: native_API=3 macro=present' in text,
        native_eagl_interposition_verified='*** EMBER-EAGL: PASS native_class interpose forward restore' in text,
        system_gpu_verified=False,backboardd_execution_verified=False,**cleanup_result(text))

def backboardd_result(text):
    loaded=re.search(r'\*\*\* EMBER-BB-LOADER: native_dependency_loaded program=backboardd dylib_cpusubtype=0x80000002 pid=(\d+) uid=(\d+) forwarding_only=1',text)
    end=re.search(r'\*\*\* EMBER-LOADER: (?:FAIL )?child_exit=(\d+)',text)
    if not loaded or not end:return None
    signal=re.search(r'\*\*\* EMBER-LOADER: child_signal=(\d+)',text)
    return dict(state='backboardd_dependency_observed',backboardd_dependency_loaded=True,
        backboardd_constructor_pid=int(loaded[1]),backboardd_constructor_uid=int(loaded[2]),
        backboardd_test_exit=int(end[1]),backboardd_test_signal=int(signal[1]) if signal else None,
        backboardd_test_stopped='*** EMBER-BB-LOADER: controlled_stop' in text,
        native_eagl_interposition_installed=True,backboardd_execution_verified=False,
        native_arm64e_abi_verified=False,system_gpu_verified=False,**cleanup_result(text),**exit_reason_result(text))
