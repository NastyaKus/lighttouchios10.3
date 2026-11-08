#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned Linux arm64e compiler/linker; no macOS, Apple SDK or firmware.

Downloads ~1.1 GB as streams, extracts only clang/headers and ld64/libtapi.
Does not execute either tool. Destination becomes visible only after both
archive sizes and SHA-256 values are verified. Needs ~240 MB free space.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import tempfile
import urllib.request

SOURCES=(
    dict(name='clang',url='https://download.swift.org/swift-6.2-release/ubuntu2204/swift-6.2-RELEASE/swift-6.2-RELEASE-ubuntu22.04.tar.gz',
        size=1006114176,sha256='717ddaf265316bd5b370fe72017f1f4f3f61ef867be1ee63f5b9d0735a95fd75',
        prefix='swift-6.2-RELEASE-ubuntu22.04/',mode='r|gz',selected_limit=240<<20),
    dict(name='linker',url='https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-x86_64.tar.xz',
        size=86163280,sha256='a72a7a577e2fbe2838b6b5e9c72034fa7d114af96f0e1d4b016f18730ce4056e',
        prefix='linux/iphone/',mode='r|xz',selected_limit=64<<20),
)

class CheckedStream:
    def __init__(self,source,expected):self.source=source;self.expected=expected;self.count=0;self.hash=hashlib.sha256()
    def read(self,n=-1):
        if n<0:n=min(1<<20,self.expected+1-self.count)
        block=self.source.read(min(n,self.expected+1-self.count));self.count+=len(block);self.hash.update(block)
        if self.count>self.expected:raise ValueError('Archive exceeds pinned size')
        return block
    def verify(self,digest):
        if self.count!=self.expected or self.hash.hexdigest()!=digest:raise ValueError('Toolchain archive size/SHA-256 mismatch')

def selected(name,source):
    if source=='clang':
        return bool(re.fullmatch(r'usr/bin/clang(?:-\d+)?',name) or
            re.fullmatch(r'usr/lib/clang/[^/]+/include/.*',name) or
            re.fullmatch(r'usr/lib/(?:libclang-cpp|libLLVM|libc\+\+|libc\+\+abi)[^/]*\.so[^/]*',name))
    return bool(re.fullmatch(r'bin/ld(?:64)?',name) or re.fullmatch(r'lib/libtapi\.so[^/]*',name))

def safe_relative(name):
    p=PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts:raise ValueError('Out-of-tree archive path')
    return p

def extract(source,dest):
    files=[];links=[];total=0
    with urllib.request.urlopen(source['url'],timeout=45) as response:
        stream=CheckedStream(response,source['size'])
        with tarfile.open(fileobj=stream,mode=source['mode']) as archive:
            for member in archive:
                if not member.name.startswith(source['prefix']):continue
                name=member.name[len(source['prefix']):]
                if not selected(name,source['name']):continue
                relative=safe_relative(name);target=dest/relative
                if member.isdir():continue
                if member.isfile():
                    total+=member.size
                    if member.size<0 or total>source['selected_limit']:raise ValueError('Selected extraction bound')
                    target.parent.mkdir(parents=True,exist_ok=True)
                    with archive.extractfile(member) as src,target.open('xb') as out:shutil.copyfileobj(src,out,1<<20)
                    target.chmod(member.mode&0o755);files.append(name)
                elif member.issym():links.append((relative,member.linkname))
                else:raise ValueError('Unsupported selected archive entry')
        while stream.read(1<<20):pass
        stream.verify(source['sha256'])
    for relative,link in links:
        target=dest/relative
        if PurePosixPath(link).is_absolute() or not (target.parent/link).resolve().is_relative_to(dest.resolve()):
            raise ValueError('Out-of-tree toolchain symlink')
        if not (target.parent/link).exists():raise ValueError('Missing selected symlink target')
        target.parent.mkdir(parents=True,exist_ok=True);target.symlink_to(link)
    return dict(name=source['name'],url=source['url'],bytes=stream.count,sha256=stream.hash.hexdigest(),selected_bytes=total,files=files)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    dest=args.out.absolute()
    if dest.exists() or dest.is_symlink():parser.error('--out must not exist')
    dest.parent.mkdir(parents=True,exist_ok=True)
    pending=Path(tempfile.mkdtemp(prefix='.ember-toolchain-',dir=dest.parent))
    try:
        facts=[]
        for source in SOURCES:
            print('Fetching pinned '+source['name'],flush=True)
            facts.append(extract(source,pending/source['name']))
        (pending/'provenance.json').write_text(json.dumps(facts,indent=2)+'\n')
        # No replacement of an existing installation, including a dangling link.
        if os.path.lexists(dest):raise ValueError('Destination appeared during extraction')
        pending.rename(dest)
        print('Verified toolchain: '+str(dest))
    finally:
        if pending.exists():shutil.rmtree(pending)
if __name__=='__main__':main()
