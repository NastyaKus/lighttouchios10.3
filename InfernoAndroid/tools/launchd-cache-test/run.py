#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent plistlib oracle for the C binary-plist editor. No Apple inputs."""
from pathlib import Path
import copy,datetime,os,plistlib,struct,subprocess,tempfile,unittest,shlex,random
PROJECT=Path(__file__).resolve().parents[2]
LABELS=['com.apple.voicemail.vmd','com.apple.CommCenter','com.apple.CommCenterMobileHelper','com.apple.CommCenterRootHelper','com.apple.locationd']
def fixture():
 jobs={'/System/Library/LaunchDaemons/'+s+'.plist':{'Label':s,'ProgramArguments':['/usr/bin/original',s],'RunAtLoad':True} for s in LABELS}
 jobs['/System/Library/LaunchDaemons/com.apple.SpringBoard.plist']={'Label':'com.apple.SpringBoard','Disabled':False,'KeepAlive':True}
 jobs['unrelated']={'Label':'example.other','Disabled':False,'RunAtLoad':False,'Nested':{'Enabled':False}}
 return {'LaunchDaemons':jobs,'Other':{'bytes':bytes(range(32)),'uid':plistlib.UID(7),'date':datetime.datetime(2020,8,12),'int':2**63+17,'real':1.25,'unicode':'привет 🌋','array':[1,False,True,None]}}
def pack(x):return plistlib.dumps(x,fmt=plistlib.FMT_BINARY,sort_keys=False)
class CacheTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='ember-bplist-');cls.root=Path(cls.temp.name);cls.bin=cls.root/'tool'
  subprocess.run([os.environ.get('CC','cc'),'-O2','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('EMBER_SANITIZE_FLAGS','')),str(PROJECT/'tools/launchd-cache-test/tool.c'),str(PROJECT/'guest/launchd-cache.c'),'-o',str(cls.bin)],check=True)
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def run_editor(self,raw):
  src=self.root/'input';out=self.root/'output';src.write_bytes(raw);out.unlink(missing_ok=True)
  r=subprocess.run([self.bin,src,out],capture_output=True,timeout=5)
  self.assertIn(r.returncode,(0,1),r.stderr)
  self.assertEqual(src.read_bytes(),raw)
  return r,out.read_bytes() if out.exists() else None
 def check(self,obj):
  r,out=self.run_editor(pack(obj));self.assertEqual(r.returncode,0,r.stderr)
  expected=copy.deepcopy(obj)
  for job in expected['LaunchDaemons'].values():
   if job.get('Label') in LABELS:job['Disabled']=True
  self.assertEqual(plistlib.loads(out),expected)
  r,twice=self.run_editor(out);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(out,twice)
  return out
 def reject(self,raw,why=None):
  r,out=self.run_editor(raw);self.assertNotEqual(r.returncode,0);self.assertIsNone(out)
  if why:self.assertIn(why.encode(),r.stderr)
 def test_add_disabled_and_preserve_all_other_values(self):self.check(fixture())
 def test_shared_false_object_remains_false_for_unrelated_jobs(self):
  x=fixture()
  for j in x['LaunchDaemons'].values():j['Disabled']=False
  self.check(x)
 def test_mix_existing_true_false_missing(self):
  x=fixture()
  for i,label in enumerate(LABELS[:3]):x['LaunchDaemons']['/System/Library/LaunchDaemons/'+label+'.plist']['Disabled']=bool(i%2)
  self.check(x)
 def test_already_done_is_byte_identical(self):
  x=fixture()
  for j in x['LaunchDaemons'].values():
   if j.get('Label') in LABELS:j['Disabled']=True
  raw=pack(x);r,out=self.run_editor(raw);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(out,raw)
 def test_labels_do_not_depend_on_filename(self):
  x=fixture();x['LaunchDaemons']={str(i):v for i,v in enumerate(x['LaunchDaemons'].values())};self.check(x)
 def test_many_objects_and_large_dictionary_count(self):
  x=fixture()
  for i in range(700):x['Other'][str(i)]={'contents':bytes([i%256])*17,'different':i}
  self.check(x)
 def test_utf16_keys_and_labels(self):
  raw=pack(fixture());osize,refs=raw[-26:-24];count,top,table=struct.unpack('>QQQ',raw[-24:])
  offsets=[int.from_bytes(raw[table+i*osize:table+(i+1)*osize],'big') for i in range(count)]
  out=bytearray(b'bplist00');new=[]
  for i,start in enumerate(offsets):
   body=raw[start:offsets[i+1] if i+1<count else table]
   if body[0]>>4==5:
    head=1
    if body[0]&15==15:head=2+(1<<(body[1]&15))
    body=bytes([body[0]+0x10])+body[1:head]+body[head:].decode('ascii').encode('utf-16be')
   new.append(len(out));out.extend(body)
  table=len(out);width=1 if table<256 else 2 if table<65536 else 4
  for o in new:out.extend(o.to_bytes(width,'big'))
  out.extend(bytes(6)+bytes([width,refs])+struct.pack('>QQQ',count,top,table))
  expected=fixture()
  for j in expected['LaunchDaemons'].values():
   if j.get('Label') in LABELS:j['Disabled']=True
  r,changed=self.run_editor(out);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(plistlib.loads(changed),expected)
 def test_target_dictionary_length_changes_from_14_to_15(self):
  x=fixture()
  for job in x['LaunchDaemons'].values():
   if job.get('Label') in LABELS:
    for i in range(11):job['Extra'+str(i)]=i
  self.check(x)
 def test_fractional_date_at_upper_supported_year_is_preserved(self):
  x=fixture();x['Other']['date']=datetime.datetime(9999,12,31,23,59,59,500000);self.check(x)
 def test_invalid_ascii_and_unpaired_utf16_are_rejected(self):
  raw=pack(fixture());bad=bytearray(raw);at=raw.index(b'example.other');bad[at]=255;self.reject(bad)
  at=raw.index('🌋'.encode('utf-16be'))
  for rel in [0,2]:
   bad=bytearray(raw);bad[at+rel:at+rel+2]=b'\0\1';self.reject(bad)
 def test_missing_target_is_not_partially_patched(self):
  x=fixture();x['LaunchDaemons'].pop('/System/Library/LaunchDaemons/'+LABELS[0]+'.plist');self.reject(pack(x),'missing target')
 def test_duplicate_label_is_rejected(self):
  x=fixture();x['LaunchDaemons']['duplicate']=copy.deepcopy(next(iter(x['LaunchDaemons'].values())));self.reject(pack(x),'duplicate target')
 def test_missing_springboard_guard_is_rejected(self):
  x=fixture();x['LaunchDaemons'].pop('/System/Library/LaunchDaemons/com.apple.SpringBoard.plist');self.reject(pack(x),'SpringBoard')
 def test_non_boolean_disabled_is_rejected(self):
  x=fixture();next(iter(x['LaunchDaemons'].values()))['Disabled']='false';self.reject(pack(x),'not a boolean')
 def test_wrong_schema_is_rejected(self):
  for x in [[],{}, {'LaunchDaemons':[]}, {'LaunchDaemons':{'wrong':False}}]:self.reject(pack(x))
 def test_xml_is_rejected_without_publishing(self):self.reject(b'<?xml version="1.0"?><plist/>','binary plist')
 def test_truncated_trailers_are_rejected(self):
  raw=pack(fixture())
  for n in [0,1,7,8,31,32,len(raw)-1,len(raw)-16,len(raw)-32]:self.reject(raw[:n])
 def test_invalid_reference_width(self):
  raw=bytearray(pack(fixture()));raw[-25]=3;self.reject(raw)
 def test_invalid_offsets_and_top_object(self):
  raw=bytearray(pack(fixture()))
  for position,value in [(-8,len(raw)),(-24,0),(-16,2**63)]:
   bad=raw.copy();bad[position:position+8 if position+8 else None]=value.to_bytes(8,'big');self.reject(bad)
 def test_exhausted_reference_width_fails_safely(self):
  x=fixture()
  for i in range(400):
   x['Other']['pad'+str(i)]=str(i)
   raw=pack(x);count=int.from_bytes(raw[-24:-16],'big');width=raw[-25]
   if width==1 and count+7>256:
    self.reject(raw,'width exhausted');return
  self.fail('fixture did not reach reference boundary')
 def test_deterministic_object_table_mutations_do_not_crash(self):
  raw=pack(fixture());rng=random.Random(42)
  for _ in range(100):
   b=bytearray(raw);pos=rng.randrange(len(b));b[pos]^=rng.randrange(1,256)
   r,out=self.run_editor(b)
   if r.returncode==0:
    try:plistlib.loads(out)
    except Exception as e:raise AssertionError(f"Accepted invalid mutation pos={pos} old={raw[pos]:02x} new={b[pos]:02x}") from e
 def test_every_byte_of_fuzzed_header_and_trailer_is_bounded(self):
  raw=pack(fixture())
  for pos in list(range(8))+list(range(len(raw)-32,len(raw))):
   b=bytearray(raw);b[pos]^=0xff
   r,out=self.run_editor(b)
   if r.returncode==0:plistlib.loads(out)
   else:self.assertIsNone(out)
if __name__=='__main__':unittest.main(verbosity=2)
