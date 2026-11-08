#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only HTTPS ZIP-member ranges for the existing Linux file/NBD adapter.

The sparse stub is a descriptor, never a usable image. No server code accepts
writes. The exact odd-sized DMG length is preserved (QEMU raw offset rejects it).
Requires requests; Apple data/cache outputs must not be redistributed.
"""
import argparse
import collections
import errno
import json
import re
from pathlib import Path
import socket
import struct
import time

import requests


class Member:
    def __init__(self, url, offset, size, chunk=4<<20, slots=16, session=None):
        if not url.startswith('https://') or offset<0 or size<=0 or not 1<=chunk<=4<<20 or not 1<=slots<=16:
            raise ValueError('Invalid member/cache bounds')
        self.url,self.offset,self.size,self.chunk,self.slots=url,offset,size,chunk,slots
        self.session=session or requests.Session()
        self.cache=collections.OrderedDict()
        self.downloaded=0

    def block(self, index):
        if index<0 or index*self.chunk>=self.size:raise ValueError('Outside member')
        if index in self.cache:
            self.cache.move_to_end(index)
            return self.cache[index]
        begin=self.offset+index*self.chunk
        count=min(self.chunk,self.size-index*self.chunk)
        if count<=0:raise ValueError('Outside member')
        for attempt in range(3):
            try:
                with self.session.get(self.url,headers={'Range':f'bytes={begin}-{begin+count-1}',
                    'Accept-Encoding':'identity'},timeout=(15,40),stream=True) as response:
                    content_range=re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)',response.headers.get('Content-Range',''))
                    if response.status_code!=206 or not content_range or tuple(map(int,content_range.groups()[:2]))!=(begin,begin+count-1) or int(content_range[3])<self.offset+self.size:
                        raise IOError(f'Unexpected HTTP range: {response.status_code}, {response.headers.get("Content-Range")}')
                    # Bound the body even if an HTTP peer returns an unlimited response.
                    data=response.raw.read(count+1)
                    if len(data)!=count:raise IOError('Short/oversized range response')
                    self.cache[index]=data;self.downloaded+=count
                    if len(self.cache)>self.slots:self.cache.popitem(last=False)
                    return data
            except (IOError,requests.RequestException):
                if attempt==2:raise
                time.sleep(attempt+1)

    def read(self, offset, count):
        if offset<0 or count<0 or offset>self.size or count>self.size-offset:
            raise ValueError('Read outside member')
        out=bytearray()
        while count:
            index,inside=divmod(offset,self.chunk)
            data=self.block(index);n=min(count,len(data)-inside)
            out.extend(data[inside:inside+n]);offset+=n;count-=n
        return bytes(out)


def exact(client, size):
    data=bytearray()
    while len(data)<size:
        part=client.recv(size-len(data))
        if not part:raise EOFError()
        data.extend(part)
    return bytes(data)


def serve_client(client, member):
    client.settimeout(120)
    client.sendall(struct.pack('>QQH',0x4e42444d41474943,0x49484156454f5054,3))
    flags,=struct.unpack('>I',exact(client,4))
    magic,option,length=struct.unpack('>QII',exact(client,16))
    if flags&~3 or magic!=0x49484156454f5054 or option!=1 or length>4096:
        raise ValueError('Unsupported NBD negotiation')
    exact(client,length)
    client.sendall(struct.pack('>QH',member.size,3)+(b'' if flags&2 else bytes(124)))
    last=0
    while True:
        # A persistent read-only export may legitimately be idle while the
        # guest/UI is stopped. Bound a partial request, not the session age.
        client.settimeout(None)
        first=exact(client,1);client.settimeout(120)
        magic,flags,command,handle,offset,count=struct.unpack('>IHHQQI',first+exact(client,27))
        if magic!=0x25609513 or flags or count>32<<20:raise ValueError('Bad NBD request')
        if command==2:return
        if command==1:
            exact(client,count)
            code=errno.EROFS;data=b''
        elif command==3:code=0;data=b''
        elif command!=0:code=errno.ENOTSUP;data=b''
        else:
            try:data=member.read(offset,count);code=0
            except ValueError:code=errno.EINVAL;data=b''
            except Exception as error:
                print('HTTP read failed:',str(error),flush=True);code=errno.EIO;data=b''
        client.sendall(struct.pack('>IIQ',0x67446698,code,handle)+data)
        if member.downloaded-last>=64<<20:
            print(json.dumps({'downloaded':member.downloaded,'cached_blocks':len(member.cache)}),flush=True)
            last=member.downloaded


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--facts',type=Path,required=True)
    parser.add_argument('--socket',type=Path,required=True)
    args=parser.parse_args();facts=json.loads(args.facts.read_text())
    if facts['method']!=0 or facts['compressed']!=facts['size']:parser.error('Member must be stored')
    member=Member(facts['url'],facts['offset'],facts['size'])
    listener=socket.socket(socket.AF_UNIX)
    listener.bind(str(args.socket)) # Never unlink another process's endpoint.
    try:
        listener.listen(2);print('READY read-only member bytes='+str(member.size),flush=True)
        while True:
            client,_=listener.accept()
            with client:
                try:serve_client(client,member)
                except (EOFError,ConnectionError,TimeoutError):pass
    finally:listener.close();args.socket.unlink()


if __name__=='__main__':main()
