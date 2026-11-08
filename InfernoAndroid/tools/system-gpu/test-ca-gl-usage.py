#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ca-gl-usage.py on hand-assembled arm64e code: attribution of macro-context
slot calls, refusal to guess, and host coverage. Needs capstone, no Apple data."""
import importlib.util, json, struct, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('ca_gl_usage', HERE / 'ca-gl-usage.py')
tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(tool)
ABI = json.loads((HERE.parents[1] / 'gpu-bridge/system/18A5351d-abi.json').read_text())
SLOTS = ABI['dispatch']['slots']

def ldr(t, n, imm): return 0xF9400000 | ((imm // 8) << 10) | (n << 5) | t
def blraaz(n): return 0xD63F081F | (n << 5)
def braaz(n): return 0xD61F081F | (n << 5)
def bl(): return 0x94000010
def ret(): return 0xD65F03C0
def code(*words): return b''.join(struct.pack('<I', w) for w in words)
def via_table(slot, call=blraaz, fn=9, table=8, this=19, offset=0x780):
    return [ldr(table, this, offset), ldr(0, table, 0), ldr(fn, table, 8 * (slot + 1)), call(fn)]

class Test(unittest.TestCase):
    def setUp(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN); self.md.detail = True
        self.names = {}
        for name, slot in SLOTS.items(): self.names.setdefault(slot, []).append(name)

    def test_attributes_calls_and_tail_calls_through_the_0x780_table(self):
        active, draw = SLOTS['_glActiveTexture'], SLOTS['_glDrawArrays']
        body = code(*via_table(active), *via_table(draw, call=braaz, fn=16))
        calls, unresolved, total = tool.scan(self.md, body, 0x1000)
        self.assertEqual([s for _, s in calls], [active, draw]); self.assertEqual((unresolved, total), ([], 2))

    def test_never_guesses_other_offsets_missing_rctx_or_clobbered_tables(self):
        body = code(
            blraaz(9),                                          # unknown target
            *via_table(5, offset=0x700),                        # not the CA macro context offset
            ldr(8, 19, 0x780), ldr(9, 8, 16), blraaz(9),        # x0 is not the table's rctx
            ldr(8, 19, 0x780), ldr(0, 8, 0), bl(), ldr(9, 8, 16), blraaz(9),  # x8 clobbered by the call
            ret(), ldr(9, 8, 16), blraaz(9))                    # state does not survive a return
        calls, unresolved, total = tool.scan(self.md, body, 0x2000)
        self.assertEqual(calls, []); self.assertEqual(total, 5); self.assertEqual(len(unresolved), 5)

    def test_function_starts_reset_state(self):
        body = code(ldr(8, 19, 0x780), ldr(0, 8, 0), ldr(9, 8, 16), blraaz(9))
        _, unresolved, _ = tool.scan(self.md, body, 0x3000, starts=[0x3008])
        self.assertEqual(len(unresolved), 1)

    def test_report_marks_covered_missing_and_private_slots(self):
        covered, missing, private = SLOTS['_glBindVertexArrayOES'], SLOTS['_glAlphaFunc'], 100000
        out = tool.report([(1, covered), (2, covered), (3, missing), (4, private)], [9], 5, self.names,
                          lambda a: '__ZN2CA3OGL11GLESContext4drawEv' if a < 3 else None)
        by = {s['slot']: s for s in out['slots']}
        self.assertEqual(by[covered]['status'], 'covered'); self.assertEqual(by[covered]['calls'], 2)
        self.assertIn('EGS_VERTEX_ARRAY_BIND', by[covered]['host']); self.assertEqual(by[covered]['gl'], ['glBindVertexArray'])
        self.assertEqual(by[missing]['status'], 'missing'); self.assertEqual(out['missing_in_host'], [['glAlphaFunc']])
        self.assertEqual(out['private_slots'], [private]); self.assertEqual(out['unresolved_call_sites'], 1)
        self.assertFalse(out['hook_manifest_verified'])

    def test_every_host_entry_point_is_a_real_public_export(self):
        exported = {tool.canonical(n) for n in SLOTS}
        unknown = sorted(n for names in tool.HOST.values() for n in names if n not in exported)
        self.assertEqual(unknown, [])

if __name__ == '__main__':
    unittest.main()
