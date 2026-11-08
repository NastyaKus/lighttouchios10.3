#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build JNI ad-hoc signer, Android arm64 or Linux verification facade.

Procursus ldid af86971ae72ec3ed3d0a699107c4e882324c941b is AGPL-3.0-only.
Static dependencies: OpenSSL 3.5.7 (Apache-2.0), libplist 2.6.0 (LGPL-2.1+).
All sources/checksums are pinned; no Apple SDK or Mac is used.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

DEPS=[('https://deb.debian.org/debian/pool/main/o/openssl/openssl_3.5.7.orig.tar.gz','a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8'),
      ('https://deb.debian.org/debian/pool/main/libp/libplist/libplist_2.6.0.orig.tar.xz','b4568add48278f97098ddac148eb3845fc45303fb34e516a7c4bb50cc785b2eb')]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--ndk',type=Path)
p.add_argument('--ldid',type=Path,required=True);p.add_argument('--java',type=Path,required=True)
p.add_argument('--openssl',type=Path,help='Configured/built OpenSSL directory to reuse')
p.add_argument('--plist-prefix',type=Path,default=Path('/usr'),help='Linux verification headers/libraries')
p.add_argument('--plist-library',type=Path,help='Linux versioned libplist if no development symlink exists')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);here=Path(__file__).resolve().parent
commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=a.ldid,text=True).strip()
if commit!='af86971ae72ec3ed3d0a699107c4e882324c941b':raise SystemExit('Unexpected ldid commit')
if subprocess.run(['git','diff','--quiet','HEAD','--'],cwd=a.ldid).returncode:raise SystemExit('Modified pinned ldid sources')
env=os.environ.copy()
if a.ndk:
    tool=a.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    cc=str(tool/'aarch64-linux-android29-clang');cxx=cc+'++';ar=str(tool/'llvm-ar')
    # Fetch once; failures never leave a file considered verified.
    for url,digest in DEPS:
        file=out/url.rsplit('/',1)[1]
        if not file.exists():
            with urllib.request.urlopen(url,timeout=45) as response,file.with_suffix(file.suffix+'.part').open('wb') as dest:
                while True:
                    chunk=response.read(1<<20)
                    if not chunk:break
                    dest.write(chunk)
            file.with_suffix(file.suffix+'.part').rename(file)
        if hashlib.sha256(file.read_bytes()).hexdigest()!=digest:raise SystemExit('Source SHA256 mismatch: '+file.name)
        directory=out/('openssl-3.5.7' if 'openssl' in file.name else 'libplist-2.6.0')
        if not directory.exists():
            with tarfile.open(file) as archive:archive.extractall(out,filter='data')
    ssl=a.openssl.resolve() if a.openssl else out/'openssl-3.5.7'
    if not (ssl/'libcrypto.a').exists():
        env.update(ANDROID_NDK_ROOT=str(a.ndk.resolve()),ANDROID_NDK_HOME=str(a.ndk.resolve()),PATH=str(tool)+os.pathsep+env['PATH'])
        subprocess.run(['perl','./Configure','android-arm64','-D__ANDROID_API__=29','no-shared','no-tests','no-apps','no-module','no-engine'],cwd=ssl,env=env,check=True)
        subprocess.run(['make','-j2','build_libs'],cwd=ssl,env=env,check=True)
    plist=out/'libplist-2.6.0';objects=[]
    sources=['base64','bytearray','hashtable','ptrarray','time64','xplist','bplist','jsmn','jplist','oplist','out-default','out-plutil','out-limd','plist']
    for source in [plist/'src'/(n+'.c') for n in sources]+[plist/'libcnary'/n for n in ['node.c','node_list.c']]:
        obj=out/(source.stem+'.o');objects.append(str(obj))
        subprocess.run([cc,'-std=gnu11','-O2','-fPIC','-DHAVE_STRNDUP','-DHAVE_STRPTIME','-DHAVE_TIMEGM','-DHAVE_TM_TM_GMTOFF','-DHAVE_TM_TM_ZONE','-DHAVE_LOCALTIME_R','-DHAVE_GMTIME_R',
            '-DPACKAGE_VERSION="2.6.0"','-I'+str(plist/'include'),'-I'+str(plist/'src'),'-I'+str(plist/'libcnary/include'),'-I'+str(plist),'-c',str(source),'-o',str(obj)],check=True)
    subprocess.run([ar,'rcs',str(out/'libplist.a'),*objects],check=True)
    includes=['-I'+str(ssl/'include'),'-I'+str(plist/'include')]
    libs=[str(out/'libplist.a'),str(ssl/'libcrypto.a'),'-ldl','-static-libstdc++','-Wl,-z,max-page-size=16384']
else:
    cxx='g++';includes=['-I'+str(a.plist_prefix/'include')]
    libs=[str(a.plist_library)] if a.plist_library else ['-L'+str(a.plist_prefix/'lib'),'-lplist-2.0']
    libs+=['-lcrypto','-ldl']
common=['-std=c++14','-O2','-fPIC','-fvisibility=hidden','-ffunction-sections','-fdata-sections',
    '-ffile-prefix-map='+str(a.ldid.resolve())+'=/ember-src/ldid',
    '-ffile-prefix-map='+str(here)+'=/ember-src/gpu-bridge','-I'+str(a.ldid.resolve()),*includes]
subprocess.run([cxx,*common,'-Dmain=ember_ldid_cli_main','-DLDID_VERSION="2.1.5-procursus7"','-c',str(a.ldid/'ldid.cpp'),'-o',str(out/'ldid.o')],check=True)
subprocess.run([cxx,*common,'-I'+str(a.java/'include'),'-I'+str(a.java/'include/linux'),'-c',str(here/'codesign-jni.cpp'),'-o',str(out/'jni.o')],check=True)
subprocess.run([cxx,'-shared',str(out/'jni.o'),str(out/'ldid.o'),*libs,'-Wl,--gc-sections','-o',str(out/'libember_codesign.so')],check=True)
if a.ndk:subprocess.run([str(tool/'llvm-strip'),'--strip-unneeded',str(out/'libember_codesign.so')],check=True)
print('Built',out/'libember_codesign.so')
