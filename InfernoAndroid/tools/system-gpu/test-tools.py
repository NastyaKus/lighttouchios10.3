#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic hostile-input checks; never downloads firmware."""
import io
import hashlib
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch
from dyld14 import Cache
from remote_member import Member,serve_client

def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),Path(__file__).with_name(name+'.py'))
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
arch_check=module('check-guest-arch');toolchain=module('fetch-toolchain')

class ToolchainTests(unittest.TestCase):
    def test_linker_cannot_silently_drop_or_use_preview_arm64e_abi(self):
        def image(subtype,filetype=2):return struct.pack('<8I',0xfeedfacf,0x100000c,subtype,filetype,0,0,0,0)
        arch_check.check(image(0x80000002),'arm64e');arch_check.check(image(0),'arm64')
        for data in [b'',image(2),image(0),image(0x81000002),image(0x80000002,1)]:
            with self.assertRaises(ValueError):arch_check.check(data,'arm64e')
    def test_pinned_stream_cannot_accept_short_long_or_corrupt_download(self):
        original=b'toolchain';digest=hashlib.sha256(original).hexdigest()
        for source in [original,original[:-1],original+b'X',b'Toolchain']:
            stream=toolchain.CheckedStream(io.BytesIO(source),len(original))
            if source==original:
                while stream.read(2):pass
                stream.verify(digest)
            else:
                with self.assertRaises(ValueError):
                    while stream.read(2):pass
                    stream.verify(digest)
        for path in ['../bin/clang','/tmp/clang','x/../../bin/clang']:
            with self.assertRaises(ValueError):toolchain.safe_relative(path)

class CacheTests(unittest.TestCase):
    def fixture(self):
        data=bytearray(0x6000);data[:16]=b'dyld_v1  arm64e\0'
        struct.pack_into('<IIII',data,16,320,1,512,1)
        struct.pack_into('<QQQII',data,320,0x180000000,0x6000,0,5,5)
        struct.pack_into('<Q',data,512,0x180001000);struct.pack_into('<I',data,536,768)
        data[768:779]=b'/Synthetic\0';data[88:104]=bytes(range(16))
        struct.pack_into('<8I',data,4096,0xfeedfacf,0x100000c,2,6,1,24,0,0)
        struct.pack_into('<II',data,4128,0x1b,24);data[4136:4152]=bytes(range(16))
        return data
    def test_valid_identity_and_vm_mapping(self):
        cache=Cache(self.fixture());self.assertEqual(cache.offset(0x180001000),4096)
        self.assertEqual(cache.image('/Synthetic')['uuid'],'00010203-0405-0607-0809-0a0b0c0d0e0f')
        for vm in [0x17fffffff,0x180006000]:
            with self.assertRaises(ValueError):cache.offset(vm)
    def test_mapping_image_and_command_bounds(self):
        for at,value in [(16,0xffffffff),(20,0),(20,33),(24,0xfffffff0),(28,0),(536,0xffffffff),(4128+4,8),(4096+20,0xffffff)]:
            with self.subTest(at=at,value=value):
                data=self.fixture();struct.pack_into('<I',data,at,value)
                with self.assertRaises(ValueError):Cache(data).image('/Synthetic')
    def test_duplicate_path_and_unterminated_string(self):
        data=self.fixture();struct.pack_into('<I',data,28,2);data[544:576]=data[512:544]
        with self.assertRaises(ValueError):Cache(data)
        data=self.fixture();data[768:768+4096]=b'x'*4096
        with self.assertRaises(ValueError):Cache(data)
    def test_local_symbols_cannot_escape_the_local_region(self):
        for fields in [(32,1,48,8,56,1),(0xffffffff,1,24,1,25,1),(24,1,40,1,41,1)]:
            data=self.fixture();struct.pack_into('<QQ',data,72,0x5000,32)
            struct.pack_into('<6I',data,0x5000,*fields)
            with self.assertRaises(ValueError):Cache(data).image('/Synthetic')

class Response:
    def __init__(self,status,content_range,data):
        self.status_code=status;self.headers={'Content-Range':content_range};self.raw=io.BytesIO(data)
    def __enter__(self):return self
    def __exit__(self,*args):pass
class Session:
    def __init__(self,reply=None):self.calls=[];self.reply=reply
    def get(self,url,**kw):
        self.calls.append(kw);begin,end=map(int,kw['headers']['Range'][6:].split('-'))
        return self.reply or Response(206,f'bytes {begin}-{end}/112',bytes(range(begin,end+1)))
class MemberTests(unittest.TestCase):
    def test_nbd_rejects_writes_and_allows_later_reads_with_no_idle_deadline(self):
        request=lambda cmd,handle,at,n:struct.pack('>IHHQQI',0x25609513,0,cmd,handle,at,n)
        data=(struct.pack('>IQII',3,0x49484156454f5054,1,0)+request(1,10,0,4)+b'NOPE'+
              request(0,11,1,3)+request(2,12,0,0))
        class Peer:
            def __init__(self):self.input=io.BytesIO(data);self.output=bytearray();self.timeouts=[]
            def recv(self,n):return self.input.read(n)
            def sendall(self,part):self.output.extend(part)
            def settimeout(self,seconds):self.timeouts.append(seconds)
        peer=Peer();member=Member('https://example.invalid/a',100,11,chunk=4,session=Session())
        serve_client(peer,member)
        self.assertEqual(peer.timeouts,[120,None,120,None,120,None,120])
        import errno
        # Newstyle hello 18B + selected export 10B, then two replies (no write).
        out=peer.output[28:]
        self.assertEqual(bytes(out[:16]),struct.pack('>IIQ',0x67446698,errno.EROFS,10))
        self.assertEqual(bytes(out[16:]),struct.pack('>IIQ',0x67446698,0,11)+bytes(range(101,104)))
    def test_cross_chunk_and_exact_odd_tail_bounded_cache(self):
        session=Session();member=Member('https://example.invalid/a',100,11,chunk=4,slots=2,session=session)
        self.assertEqual(member.read(2,9),bytes(range(102,111)));self.assertEqual(len(member.cache),2)
        self.assertEqual(member.downloaded,11)
        before=len(session.calls);self.assertEqual(member.read(8,3),bytes(range(108,111)))
        self.assertEqual(before,len(session.calls))
    def test_out_of_bounds_has_no_network_side_effect(self):
        session=Session();member=Member('https://example.invalid/a',100,11,chunk=4,session=session)
        for at,size in [(-1,1),(0,-1),(11,1),(12,0),(10,2)]:
            with self.assertRaises(ValueError):member.read(at,size)
        with self.assertRaises(ValueError):member.block(-1)
        self.assertEqual(session.calls,[])
    def test_server_cannot_silently_ignore_or_misreport_range(self):
        for status,header,body in [(200,'bytes 100-103/112',b'abcd'),(206,'bytes 101-104/112',b'abcd'),
                (206,'bytes 100-103/*',b'abcd'),(206,'bytes 100-103/104',b'abcd'),
                (206,'bytes 100-103/112',b'abc'),(206,'bytes 100-103/112',b'abcde')]:
            session=Session(Response(status,header,body));member=Member('https://example.invalid/a',100,11,chunk=4,session=session)
            with patch('remote_member.time.sleep'):
                with self.assertRaises(IOError):member.read(0,4)
            self.assertEqual(member.downloaded,0);self.assertEqual(len(member.cache),0)
            self.assertEqual(len(session.calls),3)

if __name__=='__main__':unittest.main(verbosity=2)
