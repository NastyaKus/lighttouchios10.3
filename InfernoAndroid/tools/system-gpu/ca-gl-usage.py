#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which GLI dispatch slots CoreAnimation itself calls on 18A5351d, and which
of them the EGP2 host does not execute yet.

    python3 ca-gl-usage.py --cache /private/dyld_shared_cache_arm64e \\
        --abi ../../gpu-bridge/system/18A5351d-abi.json --out /private/NEW-ca-gl-usage.json

Reads the caller's stock cache read-only (same identity checks as dyld14.py)
and writes only slot numbers, GL names, counts and QuartzCore symbol names:
no Apple code. The scan is a linear register-provenance heuristic over
QuartzCore's __text, the same pattern dyld14.py --dispatch uses for public
trampolines:

    ldr  xT, [xAny, #0x780]      ; CA GLESContext -> macro context table
    ldr  x0, [xT]                ; word 0: rctx
    ldr  xF, [xT, #8*(N+1)]      ; slot N
    blraaz xF                    ; (or braaz for a tail call)

Calls it cannot attribute (the table reloaded from a spill, another key, an
unusual register flow) are counted as unresolved, never guessed. A slot that
no public trampoline maps to is reported as private. Everything this prints is
an inventory to direct host work, not a verified hook manifest.
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dyld14  # noqa: E402  (our own module beside this file)

QUARTZCORE = '/System/Library/Frameworks/QuartzCore.framework/QuartzCore'
TABLE_OFFSET = 0x780
MAX_TEXT = 64 << 20

# EGP2 host command -> GL entry points it implements (gpu-bridge/system/commands.h).
HOST = {
    'EGS_GET_ERROR': ['glGetError'], 'EGS_GET_STRING': ['glGetString'], 'EGS_GET_INTEGER': ['glGetIntegerv'],
    'EGS_FINISH': ['glFinish'], 'EGS_FLUSH': ['glFlush'],
    'EGS_OBJECT_CREATE': ['glGenBuffers', 'glGenTextures', 'glGenFramebuffers', 'glGenRenderbuffers',
                          'glGenVertexArrays', 'glCreateShader', 'glCreateProgram'],
    'EGS_OBJECT_DELETE': ['glDeleteBuffers', 'glDeleteTextures', 'glDeleteFramebuffers', 'glDeleteRenderbuffers',
                          'glDeleteVertexArrays', 'glDeleteShader', 'glDeleteProgram'],
    'EGS_SHADER_SOURCE': ['glShaderSource'], 'EGS_SHADER_COMPILE': ['glCompileShader'],
    'EGS_SHADER_PARAMETER': ['glGetShaderiv'], 'EGS_SHADER_LOG': ['glGetShaderInfoLog'],
    'EGS_SHADER_ATTACH': ['glAttachShader'], 'EGS_SHADER_DETACH': ['glDetachShader'],
    'EGS_ATTRIB_LOCATION': ['glBindAttribLocation'], 'EGS_PROGRAM_LINK': ['glLinkProgram'],
    'EGS_PROGRAM_PARAMETER': ['glGetProgramiv'], 'EGS_PROGRAM_LOG': ['glGetProgramInfoLog'],
    'EGS_PROGRAM_USE': ['glUseProgram'], 'EGS_PROGRAM_VALIDATE': ['glValidateProgram'],
    'EGS_ATTRIB_QUERY': ['glGetAttribLocation'], 'EGS_UNIFORM_QUERY': ['glGetUniformLocation'],
    'EGS_UNIFORM_FV': ['glUniform1f', 'glUniform2f', 'glUniform3f', 'glUniform4f',
                       'glUniform1fv', 'glUniform2fv', 'glUniform3fv', 'glUniform4fv'],
    'EGS_UNIFORM_IV': ['glUniform1i', 'glUniform2i', 'glUniform3i', 'glUniform4i',
                       'glUniform1iv', 'glUniform2iv', 'glUniform3iv', 'glUniform4iv'],
    'EGS_UNIFORM_MATRIX': ['glUniformMatrix2fv', 'glUniformMatrix3fv', 'glUniformMatrix4fv'],
    'EGS_BUFFER_BIND': ['glBindBuffer'], 'EGS_BUFFER_DATA': ['glBufferData'], 'EGS_BUFFER_SUB_DATA': ['glBufferSubData'],
    'EGS_TEXTURE_ACTIVE': ['glActiveTexture'], 'EGS_TEXTURE_BIND': ['glBindTexture'],
    'EGS_TEXTURE_PARAMETER': ['glTexParameteri', 'glTexParameterf', 'glTexParameteriv', 'glTexParameterfv'],
    'EGS_TEXTURE_IMAGE': ['glTexImage2D'], 'EGS_TEXTURE_SUB_IMAGE': ['glTexSubImage2D'],
    'EGS_TEXTURE_MIPMAP': ['glGenerateMipmap'], 'EGS_TEXTURE_PARAMETER_QUERY': ['glGetTexParameteriv'],
    'EGS_RENDERBUFFER_BIND': ['glBindRenderbuffer'], 'EGS_RENDERBUFFER_STORAGE': ['glRenderbufferStorage'],
    'EGS_FRAMEBUFFER_BIND': ['glBindFramebuffer'], 'EGS_FRAMEBUFFER_TEXTURE': ['glFramebufferTexture2D'],
    'EGS_FRAMEBUFFER_RENDERBUFFER': ['glFramebufferRenderbuffer'],
    'EGS_FRAMEBUFFER_STATUS': ['glCheckFramebufferStatus'], 'EGS_DRAW_BUFFERS': ['glDrawBuffers'],
    'EGS_BLIT_FRAMEBUFFER': ['glBlitFramebuffer'], 'EGS_READ_BUFFER': ['glReadBuffer'],
    'EGS_VERTEX_ARRAY_BIND': ['glBindVertexArray'], 'EGS_ATTRIB_POINTER': ['glVertexAttribPointer'],
    'EGS_ATTRIB_ENABLE': ['glEnableVertexAttribArray', 'glDisableVertexAttribArray'],
    'EGS_ATTRIB_DIVISOR': ['glVertexAttribDivisor'],
    'EGS_ATTRIB_VALUE': ['glVertexAttrib1f', 'glVertexAttrib2f', 'glVertexAttrib3f', 'glVertexAttrib4f',
                         'glVertexAttrib1fv', 'glVertexAttrib2fv', 'glVertexAttrib3fv', 'glVertexAttrib4fv'],
    'EGS_DRAW_ARRAYS': ['glDrawArrays', 'glDrawArraysInstanced'],
    'EGS_DRAW_ELEMENTS': ['glDrawElements', 'glDrawElementsInstanced'],
    'EGS_VIEWPORT': ['glViewport'], 'EGS_SCISSOR': ['glScissor'], 'EGS_ENABLE': ['glEnable', 'glDisable'],
    'EGS_CLEAR_COLOR': ['glClearColor'], 'EGS_CLEAR': ['glClear'], 'EGS_COLOR_MASK': ['glColorMask'],
    'EGS_BLEND_FUNC': ['glBlendFunc', 'glBlendFuncSeparate'],
    'EGS_BLEND_EQUATION': ['glBlendEquation', 'glBlendEquationSeparate'], 'EGS_BLEND_COLOR': ['glBlendColor'],
    'EGS_DEPTH_FUNC': ['glDepthFunc'], 'EGS_DEPTH_MASK': ['glDepthMask'], 'EGS_DEPTH_RANGE': ['glDepthRangef'],
    'EGS_STENCIL_FUNC': ['glStencilFunc', 'glStencilFuncSeparate'],
    'EGS_STENCIL_OP': ['glStencilOp', 'glStencilOpSeparate'],
    'EGS_STENCIL_MASK': ['glStencilMask', 'glStencilMaskSeparate'], 'EGS_CULL_FACE': ['glCullFace'],
    'EGS_FRONT_FACE': ['glFrontFace'], 'EGS_LINE_WIDTH': ['glLineWidth'], 'EGS_POLYGON_OFFSET': ['glPolygonOffset'],
    'EGS_CLEAR_DEPTH': ['glClearDepthf'], 'EGS_CLEAR_STENCIL': ['glClearStencil'], 'EGS_READ_PIXELS': ['glReadPixels'],
}
SUFFIX = re.compile(r'(OES|EXT|APPLE|ARB)$')


def canonical(name):
    """_glBindVertexArrayOES -> glBindVertexArray."""
    return SUFFIX.sub('', name.lstrip('_'))


def host_coverage():
    covered = {}
    for command, names in HOST.items():
        for name in names:
            covered.setdefault(name, []).append(command)
    return covered


def scan(md, code, base, table_offset=TABLE_OFFSET, starts=()):
    """Linear provenance scan of one code range. Returns (calls, unresolved, total)
    where calls is a list of (address, slot). `starts` resets state at function
    entry points; any unconditional branch or return also resets it."""
    from capstone.arm64 import ARM64_OP_MEM, ARM64_OP_REG
    starts = set(starts)
    regs, calls, unresolved, total = {}, [], [], 0

    def canon(reg):
        name = md.reg_name(reg)
        return 'x' + name[1:] if name.startswith('w') and name[1:].isdigit() else name

    def tag(reg):
        return regs.get(canon(reg), ('initial', canon(reg)))

    for ins in md.disasm(code, base):
        if ins.address in starts:
            regs = {}
        if ins.mnemonic in ('blraaz', 'braaz'):
            total += 1
            function, context = tag(ins.operands[0].reg), regs.get('x0')
            slot = None
            if function[0] == 'load' and function[2] > 0 and function[2] % 8 == 0:
                table = function[1]
                is_table = table[0] == 'load' and table[2] == table_offset
                rctx = context is not None and context[0] == 'load' and context[1] == table and context[2] == 0
                if is_table and rctx:
                    slot = function[2] // 8 - 1
            if slot is None:
                unresolved.append(ins.address)
            else:
                calls.append((ins.address, slot))
        result = None
        if ins.mnemonic == 'ldr' and len(ins.operands) == 2 and ins.operands[1].type == ARM64_OP_MEM:
            mem = ins.operands[1].mem
            if not mem.index and not getattr(ins, 'writeback', False):
                result = ('load', tag(mem.base), mem.disp)
        elif ins.mnemonic == 'mov' and len(ins.operands) == 2 and ins.operands[1].type == ARM64_OP_REG:
            result = tag(ins.operands[1].reg)
        for written in ins.regs_access()[1]:
            regs[canon(written)] = ('changed', ins.address, canon(written))
        if result is not None:
            regs[canon(ins.operands[0].reg)] = result
        if ins.mnemonic in ('ret', 'b', 'br', 'braaz', 'retaa', 'retab') or ins.mnemonic.startswith('bra'):
            regs = {}
        elif ins.mnemonic.startswith('bl'):
            # A call clobbers the caller-saved registers.
            for r in [f'x{i}' for i in range(19)] + ['x30']:
                regs.pop(r, None)
    return calls, unresolved, total


def report(calls, unresolved, total, slot_names, function_of=lambda a: None):
    covered = host_coverage()
    used = {}
    for address, slot in calls:
        entry = used.setdefault(slot, {'slot': slot, 'calls': 0, 'functions': set()})
        entry['calls'] += 1
        fn = function_of(address)
        if fn:
            entry['functions'].add(fn)
    slots = []
    for slot in sorted(used):
        names = sorted(slot_names.get(slot, []))
        canon = sorted({canonical(n) for n in names})
        commands = sorted({c for n in canon for c in covered.get(n, [])})
        slots.append({'slot': slot, 'gl': canon, 'exports': names, 'calls': used[slot]['calls'],
                      'functions': sorted(used[slot]['functions'])[:20],
                      'host': commands, 'status': 'private' if not names else ('covered' if commands else 'missing')})
    return {
        'call_sites_through_blraaz_or_braaz': total,
        'attributed_to_macro_context_slots': len(calls),
        'unresolved_call_sites': len(unresolved),
        'slots': slots,
        'missing_in_host': [s['gl'] for s in slots if s['status'] == 'missing'],
        'private_slots': [s['slot'] for s in slots if s['status'] == 'private'],
        'note': 'Heuristic inventory for host work. Client-side arrays reach the host as EGS_DRAW_CLIENT; '
                'a covered name is not proof that every parameter combination CoreAnimation uses is supported.',
        'hook_manifest_verified': False, 'system_gpu_verified': False,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cache', type=Path, required=True)
    p.add_argument('--abi', type=Path, default=Path(__file__).resolve().parents[2] / 'gpu-bridge/system/18A5351d-abi.json')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    if a.out.exists():
        raise SystemExit('Refusing to overwrite an existing output file')
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    abi = json.loads(a.abi.read_text())
    slot_names = {}
    for name, slot in abi['dispatch']['slots'].items():
        slot_names.setdefault(slot, []).append(name)
    with open(a.cache, 'rb') as f:
        import mmap
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        cache = dyld14.Cache(data)
        if cache.uuid != dyld14.EXPECTED_UUID or cache.uuid != abi['cache_uuid']:
            raise SystemExit('Cache UUID is not the verified 18A5351d input')
        image = cache.image(QUARTZCORE)
        text = [(vm, size) for name, vm, size in image['sections'] if name == '__text']
        if len(text) != 1 or text[0][1] > MAX_TEXT:
            raise SystemExit('Unexpected QuartzCore __text section')
        vm, size = text[0]
        md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
        md.detail = True
        md.skipdata = True
        symbols = sorted((v, n) for n, v in image['symbols'].items() if vm <= v < vm + size)
        starts = [v for v, _ in symbols]
        import bisect

        def function_of(address):
            i = bisect.bisect_right(starts, address) - 1
            return symbols[i][1] if i >= 0 else None

        calls, unresolved, total = scan(md, cache.read(cache.offset(vm, size), size), vm, starts=starts)
        out = report(calls, unresolved, total, slot_names, function_of)
        out.update({'build': '18A5351d', 'cache_uuid': cache.uuid, 'quartzcore_uuid': image['uuid'],
                    'quartzcore_text_bytes': size, 'macro_context_offset': hex(TABLE_OFFSET)})
    a.out.write_text(json.dumps(out, indent=2, sort_keys=True) + '\n')
    print(f"{out['attributed_to_macro_context_slots']} of {total} authenticated indirect calls attributed; "
          f"{len(out['slots'])} slots, missing in host: {len(out['missing_in_host'])}, private: {len(out['private_slots'])}")


if __name__ == '__main__':
    main()
