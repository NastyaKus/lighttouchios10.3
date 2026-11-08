#!/usr/bin/env python3
"""Small synthetic fixtures. Contains no Apple firmware, key or real ticket."""
import io, json, plistlib, struct, zipfile, zlib
from pathlib import Path
out = Path(__file__).resolve().parents[1] / 'app/src/test/resources/restore'
out.mkdir(parents=True, exist_ok=True)

def tlv(tag, data):
    n=len(data); length=bytes([n]) if n<128 else bytes([0x80+(n.bit_length()+7)//8])+n.to_bytes((n.bit_length()+7)//8,'big')
    return tag+length+data

def seq(*c): return tlv(b'\x30', b''.join(c))
def aset(*c): return tlv(b'\x31', b''.join(c))
def text(s): return tlv(b'\x16',s.encode())
def octet(b): return tlv(b'\x04',b)
def integer(n):
    b=n.to_bytes(max(1,(n.bit_length()+7)//8),'big')
    return tlv(b'\x02',b'\0'+b if b[0]&128 else b)
def tag(s,*nodes):
    n=int.from_bytes(s.encode(),'big');digits=[n&127];n>>=7
    while n: digits.insert(0,128|(n&127));n>>=7
    return tlv(b'\xff'+bytes(digits),seq(*nodes))
def field(k,v): return tag(k,text(k),v)
def component(k): return tag(k,text(k),aset(field('DGST',octet(b'old-digest'))))
parts=[tag('MANP',text('MANP'),aset(field('CHIP',integer(1)),field('ECID',integer(42)),field('snon',octet(b'originalnonce')))),component('rosi'),component('krnl'),component('dtre'),tag('rfta',text('rfta'),aset())]
# Certificate is intentionally synthetic; extensions[4] holds duplicate MANB.
inner=aset(*parts)
extensions=[seq(tlv(b'\x06',b'\x2a\x03'),octet(b'other')) for _ in range(4)]+[seq(tlv(b'\x06',b'\x2a\x04'),octet(inner))]
cert=seq(seq(tlv(b'\xa3',seq(*extensions))),seq(),tlv(b'\x03',b'\0fake-signature'))
ticket=seq(text('IM4M'),integer(0),aset(tag('MANB',text('MANB'),aset(*parts))),octet(b'fake-signature'),seq(cert))
identity={'Info':{'DeviceClass':'n104ap','Variant':'Research Developer Erase'},'Manifest': {k:{'Digest': bytes([i])*20,'Info':{'Path':v}} for i,(k,v) in enumerate([('OS','system.dmg'),('KernelCache','kernel.im4p'),('DeviceTree','tree.im4p')],1)}}
(out/'ticket.shsh2').write_bytes(plistlib.dumps({'ApImg4Ticket':ticket}))
(out/'identity.plist').write_bytes(plistlib.dumps(identity))
(out/'payload.im4p').write_bytes(seq(text('IM4P'),text('krnl'),text('test'),octet(b'payload\0\xff')))
with zipfile.ZipFile(out/'small.zip','w') as z:
    z.writestr('stored',b'0123456789',compress_type=zipfile.ZIP_STORED)
    z.writestr('deflated',b'hello'*2000,compress_type=zipfile.ZIP_DEFLATED)
    z.writestr('BuildManifest.plist',plistlib.dumps({'ProductBuildVersion':'18A5351d','BuildIdentities':[identity]}))
# ZIP64 with sentinel sizes AND ZIP64 end/locator, even though payload is small.
name=b'os.dmg';data=b'zip64-data';crc=zlib.crc32(data);extra=struct.pack('<HHQQ',1,16,len(data),len(data))
local=struct.pack('<IHHHHHIIIHH',0x04034b50,45,0,0,0,0,crc,0xffffffff,0xffffffff,len(name),len(extra))+name+extra+data
extra=struct.pack('<HHQQQ',1,24,len(data),len(data),0)
central=struct.pack('<IHHHHHHIIIHHHHHII',0x02014b50,45,45,0,0,0,0,crc,0xffffffff,0xffffffff,len(name),len(extra),0,0,0,0,0xffffffff)+name+extra
end64=struct.pack('<IQHHIIQQQQ',0x06064b50,44,45,45,0,0,1,1,len(central),len(local))
locator=struct.pack('<IIQI',0x07064b50,0,len(local)+len(central),1)
end=struct.pack('<IHHHHIIH',0x06054b50,0,0,65535,65535,0xffffffff,0xffffffff,0)
(out/'zip64.zip').write_bytes(local+central+end64+locator+end)
# A raw synthetic HFSX volume, 512-byte allocation blocks; catalog has a header and one leaf.
volume=bytearray(256*512)
def w16(p,v): struct.pack_into('>H',volume,p,v)
def w32(p,v): struct.pack_into('>I',volume,p,v)
def w64(p,v): struct.pack_into('>Q',volume,p,v)
def fork(p,size,start,blocks):
    w64(p,size);w32(p+12,blocks);w32(p+16,start);w32(p+20,blocks)
volume[1024:1026]=b'HX';w16(1026,5);w32(1064,512);w32(1068,256);w32(1072,212)
fork(1024+112,32,3,1);fork(1024+272,16384,4,32)
for b in range(44): volume[3*512+b//8] |= 128>>(b%8)
cat=4*512;w32(cat+24,1);w16(cat+32,8192)
leaf=cat+8192;volume[leaf+8]=255
paths=['/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-01.30-006-P.bin','/usr/standalone/firmware/nfrestore/firmware/jcop-prod/JCOP-11.04-012.2-P.bin','/System/Library/LaunchDaemons/com.apple.syslogd.plist']
folders={'/':2};entries=[];cnid=16;locations={}
for path in paths:
    parent='/'
    for name in path.strip('/').split('/')[:-1]:
        full=parent.rstrip('/')+'/'+name
        if full not in folders:
            folders[full]=cnid;cnid+=1;entries.append((folders[parent],name,1,folders[full],full))
        parent=full
    entries.append((folders[parent],path.split('/')[-1],2,cnid,path));cnid+=1
pos=14
for parent,name,kind,nodeid,path in sorted(entries):
    index=len(locations);locations[path]=leaf+pos
    name16=name.encode('utf-16-be');kl=6+len(name16);key=struct.pack('>HIH',kl,parent,len(name))+name16
    p=leaf+pos;volume[p:p+len(key)]=key;record=p+len(key)
    w16(record,kind);w32(record+8,nodeid)
    if kind==2:
        w32(record+32,501);w32(record+36,20);volume[record+41]=32;w16(record+42,0x81a4)
        if path==paths[0]: fork(record+88,1024,36,4)
        if path==paths[1]: fork(record+88,1024,40,4)
    w16(leaf+8192-(index+1)*2,pos);pos+=len(key)+(248 if kind==2 else 88)
w16(leaf+10,len(entries));w16(leaf+8192-(len(entries)+1)*2,pos)
(out/'ramdisk.im4p').write_bytes(seq(text('IM4P'),text('rdsk'),text('synthetic'),octet(volume)))
(out/'hfs-locations.json').write_text(json.dumps(locations,indent=2))
# Separate fixture: retain the original parity volume byte-for-byte, but add
# the compressed/empty-fork launchd record used by an isolated patch boot.
for extra,name in enumerate(['com.apple.restored_external.plist','com.apple.KeyMaker.plist']):
    name16=name.encode('utf-16-be')
    key=struct.pack('>HIH',6+len(name16),folders['/System/Library/LaunchDaemons'],len(name))+name16
    p=leaf+pos;volume[p:p+len(key)]=key;record=p+len(key)
    w16(record,2);w32(record+8,cnid+extra);w32(record+32,501);w32(record+36,20)
    volume[record+41]=32;w16(record+42,0x81a4)
    w16(leaf+8192-(len(entries)+extra+1)*2,pos);pos+=len(key)+248
    (out/('patchboot-record-offset.txt' if extra==0 else 'patchboot-storage-offset.txt')).write_text(str(record)+'\n')
w16(leaf+10,len(entries)+2);w16(leaf+8192-(len(entries)+3)*2,pos)
(out/'patchboot-stock.im4p').write_bytes(seq(text('IM4P'),text('rdsk'),text('synthetic'),octet(volume)))
print('Synthetic fixtures:',out)
