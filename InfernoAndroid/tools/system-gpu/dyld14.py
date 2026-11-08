#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded read-only analysis of the exact iOS 14 arm64e shared cache.

Outputs identities, symbol addresses and GL dispatch observations, never a
redistributable Apple binary. A symbol map is not a validated hook manifest.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct
import uuid

EXPECTED_UUID='11ad39c9-e3c9-3067-a337-7287744fdcb5'


class Cache:
    def __init__(self, data):
        self.data=data
        if bytes(data[:16])!=b'dyld_v1  arm64e\0':raise ValueError('Expected legacy iOS arm64e cache')
        table,count=self.u32(16),self.u32(20)
        if not 1<=count<=32:raise ValueError('Bad mapping count')
        self.bounds(table,count*32)
        self.maps=[self.unpack('<QQQII',table+i*32) for i in range(count)]
        for vm,size,offset,_,_ in self.maps:
            if not size or vm+size>1<<64:raise ValueError('Bad VM mapping')
            self.bounds(offset,size)
        for ranges in [[(m[0],m[1]) for m in self.maps],[(m[2],m[1]) for m in self.maps]]:
            ordered=sorted(ranges)
            if any(a+n>b for (a,n),(b,_) in zip(ordered,ordered[1:])):raise ValueError('Overlapping mappings')
        table,count=self.u32(24),self.u32(28)
        if not 1<=count<=20000:raise ValueError('Bad image count')
        self.bounds(table,count*32)
        self.images={}
        for i in range(count):
            at=table+i*32;vm=self.u64(at);name=self.cstr(self.u32(at+24))
            self.offset(vm,32)
            if name in self.images:raise ValueError('Duplicate image path')
            self.images[name]=vm
        self.uuid=str(uuid.UUID(bytes=self.read(88,16)))

    def bounds(self, at, size):
        if at<0 or size<0 or at>len(self.data) or size>len(self.data)-at:raise ValueError('Cache bounds')

    def read(self, at, size):self.bounds(at,size);return bytes(self.data[at:at+size])
    def unpack(self, fmt, at):return struct.unpack(fmt,self.read(at,struct.calcsize(fmt)))
    def u32(self, at):return self.unpack('<I',at)[0]
    def u64(self, at):return self.unpack('<Q',at)[0]

    def cstr(self, at, limit=4096):
        self.bounds(at,1);end=self.data.find(b'\0',at,min(len(self.data),at+limit))
        if end<0:raise ValueError('Unterminated cache string')
        return self.read(at,end-at).decode('utf-8')

    def offset(self, vm, size=1):
        if vm<0 or vm>=1<<64 or size<0:raise ValueError('Invalid VM range')
        for start,length,offset,_,_ in self.maps:
            if start<=vm and vm-start<=length and size<=length-(vm-start):return offset+vm-start
        raise ValueError('Unmapped VM address '+hex(vm))

    def pointer(self, vm):
        # dyld chained slide metadata occupies the high bits. These are data
        # pointers, not callable PAC pointers. Never use this for runtime IMPs.
        value=self.u64(self.offset(vm,8)) & ((1<<48)-1)
        self.offset(value)
        return value

    def methods(self, vm):
        at=self.offset(vm,8);flags,count=self.unpack('<II',at)
        if flags&0xc0000000!=0xc0000000 or flags&0x3ffffffc!=12 or count>4096:
            raise ValueError('Unsupported Objective-C method list')
        result=[]
        for i in range(count):
            entry=vm+8+i*12;name,types,imp=self.unpack('<iii',self.offset(entry,12))
            self.offset(entry+8+imp)
            result.append({'selector':self.cstr(self.offset(entry+name)),
                'encoding':self.cstr(self.offset(entry+4+types)), 'imp':hex(entry+8+imp)})
        return result

    def image(self, name):
        vm=self.images[name];start=self.offset(vm,32)
        magic,_,_,filetype,ncmds,cmdsize,_,_=self.unpack('<8I',start)
        if magic!=0xfeedfacf or filetype!=6 or ncmds>4096 or cmdsize>4<<20:raise ValueError('Invalid Mach-O image')
        commands=[];at=start+32;end=at+cmdsize
        self.bounds(at,cmdsize)
        for _ in range(ncmds):
            cmd,size=self.unpack('<II',at)
            if size<8 or size%4 or size>end-at:raise ValueError('Invalid load command')
            commands.append((cmd,at,size));at+=size
        if at!=end:raise ValueError('Load command size mismatch')
        identifiers=[str(uuid.UUID(bytes=self.read(at+8,16))) for cmd,at,size in commands if cmd==0x1b and size==24]
        if len(identifiers)!=1 or sum(cmd==0x1b for cmd,_,_ in commands)!=1:raise ValueError('Invalid image UUID')
        ident=identifiers[0]
        symbols={};sections=[]
        for cmd,at,size in commands:
            if cmd==2:
                if size!=24:raise ValueError('Invalid symbol command')
                table,count,strings,string_size=self.unpack('<4I',at+8)
                self.bounds(strings,string_size)
                if count>1_000_000:raise ValueError('Symbol count')
                self.bounds(table,count*16)
                for i in range(count):
                    nameoff,kind,sect,desc,value=self.unpack('<IBBHQ',table+i*16)
                    if nameoff>=string_size:raise ValueError('Symbol name bounds')
                    if value and kind&0xe==0xe:symbols[self.cstr(strings+nameoff,min(4096,string_size-nameoff))]=value
            if cmd==0x19:
                if size<72:raise ValueError('Invalid segment command')
                count=self.u32(at+64)
                if count>128 or 72+count*80>size:raise ValueError('Section bounds')
                for i in range(count):
                    entry=at+72+i*80
                    sect=self.read(entry,16).split(b'\0')[0].decode()
                    addr,length=self.unpack('<QQ',entry+32)
                    sections.append((sect,addr,length))
        # Local symbols describe original image symbols stripped from LC_SYMTAB.
        local=self.u64(72);local_size=self.u64(80)
        if local:
            self.bounds(local,local_size)
            if local_size<24:raise ValueError('Local symbol header bounds')
            nlist,ncount,strings,string_size,entries,ecount=self.unpack('<6I',local)
            if ncount>10_000_000 or ecount>20000:raise ValueError('Local symbol bounds')
            for offset,size in [(nlist,ncount*16),(strings,string_size),(entries,ecount*12)]:
                if offset<24 or offset>local_size or size>local_size-offset:raise ValueError('Local symbol region bounds')
            for i in range(ecount):
                imageoff,first,count=self.unpack('<III',local+entries+i*12)
                if imageoff!=start:continue
                if first>ncount or count>ncount-first:raise ValueError('Local nlist range')
                for j in range(first,first+count):
                    nameoff,kind,sect,desc,value=self.unpack('<IBBHQ',local+nlist+j*16)
                    if nameoff>=string_size:raise ValueError('Local string bounds')
                    if value and kind&0xe==0xe:symbols[self.cstr(local+strings+nameoff,min(4096,string_size-nameoff))]=value
        return {'path':name,'uuid':ident,'vm':vm,'symbols':symbols,'sections':sections}


def inspect(cache):
    if cache.uuid!=EXPECTED_UUID:raise ValueError('Cache UUID is not the verified 18A5351d input')
    images=[]
    for path in cache.images:
        if path.endswith(('/QuartzCore.framework/QuartzCore','/OpenGLES.framework/OpenGLES','/GLEngine.bundle/GLEngine','/libGFXShared.dylib')):
            image=cache.image(path)
            wanted={n:hex(v) for n,v in image['symbols'].items() if n.startswith(('_gl','_eagl','_gli')) or
                ('CA' in n and any(s in n for s in ['EAGLServer','GLESContext','AsynchronousDispatcher','CAMetalContext']))}
            selectors=set()
            for name,vm,size in image['sections']:
                if name=='__objc_methname':
                    if size>16<<20:raise ValueError('Selector section too large')
                    selectors.update(s.decode() for s in cache.read(cache.offset(vm,size),size).split(b'\0') if s)
                if name=='__objc_selrefs':
                    if size>1<<20 or size%8:raise ValueError('Selector reference bounds')
                    for pos in range(0,size,8):selectors.add(cache.cstr(cache.offset(cache.pointer(vm+pos))))
            methods=[]
            if path.endswith('/OpenGLES.framework/OpenGLES'):
                for key in ['__OBJC_$_INSTANCE_METHODS_EAGLContext','__OBJC_$_CLASS_METHODS_EAGLContext']:
                    methods+=cache.methods(image['symbols'][key])
            images.append({'path':path,'uuid':image['uuid'],'vm':hex(image['vm']),
                'symbols':wanted,'eagl_methods':methods,'eagl_selectors':sorted(s for s in selectors if any(x in s for x in ['MacroContext','initWithAPI','CoreSurface','IOSurface','Parameter:','currentContext','CurrentContext']))})
    return {'build':'18A5351d','cache_uuid':cache.uuid,'images':images,
        'hook_manifest_verified':False,'note':'Identity/symbol inventory only. Dispatch layout, calling conventions and original patch bytes still require validation.'}


def dispatch_inventory(cache):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    from capstone.arm64 import ARM64_OP_MEM, ARM64_OP_REG
    md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN);md.detail=True
    image=cache.image('/System/Library/Frameworks/OpenGLES.framework/OpenGLES')
    symbols=image['symbols'];addresses=sorted(set(symbols.values()));slots={};unmapped=[]
    for name,vm in sorted(symbols.items()):
        if not name.startswith('_gl') or len(name)<4 or not name[3].isupper():continue
        end=next((a for a in addresses if a>vm),vm+96);size=min(end-vm,128)
        code=list(md.disasm(cache.read(cache.offset(vm,size),size),vm))
        found=[];regs={}
        def canon(reg):
            reg=md.reg_name(reg)
            return 'x'+reg[1:] if reg.startswith('w') and reg[1:].isdigit() else reg
        def tag(reg):return regs.get(canon(reg),('initial',canon(reg)))
        for ins in code:
            if ins.mnemonic in ('braaz','blraaz'):
                function=tag(ins.operands[0].reg);context=regs.get('x0')
                if context and function[0]=='load' and context[0]=='load' and function[1]==context[1] and context[2]==32 and function[2]>32 and function[2]%8==0:
                    found.append((function[2]-32)//8-1)
            result=None
            if ins.mnemonic=='ldr' and len(ins.operands)==2 and ins.operands[1].type==ARM64_OP_MEM:
                mem=ins.operands[1].mem
                if not mem.index:result=('load',tag(mem.base),mem.disp)
            if ins.mnemonic=='mov' and len(ins.operands)==2 and ins.operands[1].type==ARM64_OP_REG:
                result=tag(ins.operands[1].reg)
            for written in ins.regs_access()[1]:regs[canon(written)]=('changed',ins.address,canon(written))
            if result is not None:regs[canon(ins.operands[0].reg)]=result
        if len(set(found))==1:slots[name]=found[0]
        else:unmapped.append(name)
    return {'pointer_auth':'braaz/blraaz: instruction key A, discriminator zero',
        'macro_context_first_argument_offset':0,'public_dispatch_context_offset':32,
        'slots':slots,'unmapped_exports':unmapped,
        'private_slots_and_complete_coreanimation_coverage_verified':False}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--dispatch',action='store_true',help='Also inspect public GL trampolines; requires capstone 5.0.6')
    a=p.parse_args()
    with a.cache.open('rb') as file,mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        result=inspect(Cache(data));result['cache_sha256']=hashlib.sha256(data).hexdigest()
        if a.dispatch:result['dispatch']=dispatch_inventory(Cache(data))
    with a.out.open('x') as output:json.dump(result,output,indent=2);output.write('\n')
    print(json.dumps({'cache_uuid':result['cache_uuid'],'images':[(v['path'],len(v['symbols'])) for v in result['images']]}))


if __name__=='__main__':main()
