#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the synthetic C guest fixture, independently using Python plistlib."""
from pathlib import Path
from importlib.util import spec_from_file_location,module_from_spec
root=Path(__file__).resolve().parent
spec=spec_from_file_location('synthetic_cache',root/'run.py');m=module_from_spec(spec);spec.loader.exec_module(m)
b=m.pack(m.fixture())
s='// SPDX-License-Identifier: GPL-3.0-or-later\n// Synthetic plistlib fixture. No Apple file contents.\nstatic const unsigned char launchd_fixture[] = {\n'
for i in range(0,len(b),20):s+=' '+','.join('0x%02x'%v for v in b[i:i+20])+',\n'
s+='};\n'
(root.parent/'guest-daemon-test/launchd-fixture.h').write_text(s)
