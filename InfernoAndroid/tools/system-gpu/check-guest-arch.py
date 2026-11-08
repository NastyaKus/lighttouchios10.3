#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject silent arm64 fallback and the pre-iOS14 preview arm64e ABI."""
import struct
import sys
from pathlib import Path

def check(data, arch):
    expected={'arm64':0,'arm64e':0x80000002}[arch]
    if len(data)<32:raise ValueError('Truncated Mach-O')
    magic,cpu,subtype,filetype=struct.unpack_from('<4I',data)
    if magic!=0xfeedfacf or cpu!=0x100000c or subtype!=expected or filetype not in (2,6):
        raise ValueError(f'Expected {arch} subtype 0x{expected:08x}, got 0x{subtype:08x}, type={filetype}')

if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('check-guest-arch.py arm64|arm64e executable-or-dylib')
    check(Path(sys.argv[2]).read_bytes(),sys.argv[1])
