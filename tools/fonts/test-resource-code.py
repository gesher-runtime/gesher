"""Synthetic bytes/metadata ONLY; never opens a real PE/provider/tool."""
import copy
import hashlib
import io
import json
import runpy
import struct
import unittest
from pathlib import Path

ROOT=Path(__file__).parent
p=runpy.run_path(str(ROOT/'resource-code-provider.py'))
e=runpy.run_path(str(ROOT/'resource-code-envelope.py'))
fixture=runpy.run_path(str(ROOT/'test-resource-pe-imports.py'))
legacy=e['load']('resource-import-inventory');pe=fixture['p']

def image():
    data=fixture['image'](regular=False,delay=False)
    struct.pack_into('<I',data,fixture['TABLE']+36,0x60000020)
    return data

def provider_fixture():
    data=bytes(image());digest=hashlib.sha256(data).hexdigest()
    return data, {'role':p['ROLE'],'name':'ucrtbase.dll','bytes':len(data),
                  'sha256':digest,'version':'SYNTHETIC_ONLY'}

class Code(unittest.TestCase):
    def reject(self, fn):
        with self.assertRaises((ValueError,AssertionError,KeyError,TypeError)):
            fn()
    def test_mapping_guard_edges(self):
        data=bytes(image());row=p['window'](data,0x1000)
        self.assertEqual(row['file_offset'],fixture['RAW']);self.assertEqual(row['cache_length'],111)
        self.assertEqual(row['cache_hex'],data[fixture['RAW']:fixture['RAW']+111].hex())
        p['window'](data,0x2000-111)
        for start in (-1,True,0,0x2000-110,0xffffffff-110,1<<32):
            self.reject(lambda:p['window'](data,start))
    def test_malformed_headers(self):
        for offset,fmt,value in ((0,'H',0),(60,'I',0),(fixture['PE'],'I',0),
                (fixture['PE']+4,'H',0x14c),(fixture['PE']+6,'H',0),
                (fixture['PE']+6,'H',97),(fixture['PE']+20,'H',111),
                (fixture['OPT'],'H',0x10b),(fixture['OPT']+56,'I',0x106e),
                (fixture['OPT']+60,'I',1),(fixture['TABLE']+36,'I',0x40000040),
                (fixture['TABLE']+12,'I',0xfffffff0),
                (fixture['TABLE']+20,'I',0xfffffff0)):
            data=image();struct.pack_into('<'+fmt,data,offset,value)
            self.reject(lambda:p['window'](bytes(data),0x1000))
        for size in (0,63,127,fixture['TABLE']+39,len(image())-1):
            self.reject(lambda:p['window'](bytes(image()[:size]),0x1000))
    def test_ambiguous_virtual_raw_and_zero_fill(self):
        for rva,raw in ((0x1000,0x1400),(0x3000,0x400)):
            data=image();data.extend(b'\0'*0x1000)
            struct.pack_into('<H',data,fixture['PE']+6,2)
            struct.pack_into('<IIII',data,fixture['TABLE']+48,0x1000,rva,0x1000,raw)
            struct.pack_into('<I',data,fixture['TABLE']+76,0x60000020)
            self.reject(lambda:p['window'](bytes(data),0x1000))
        data=image();struct.pack_into('<I',data,fixture['TABLE']+16,32)
        self.reject(lambda:p['window'](bytes(data),0x1000))
    def test_profiles_are_exact(self):
        data,entry=provider_fixture();context={'source':'a'*40,'run':'1','attempt':'1'}
        self.reject(lambda:p['admit'](data,entry,context)) # Never production-admit synthetic SHA.
        # Explicit injected SYNTHETIC profile only for branch/transport tests.
        profiles=p['admit'].__globals__['PROFILES'];original=dict(profiles)
        try:
            profiles[entry['sha256']]=('SYNTHETIC_ONLY',0x1000,0x100000)
            record=p['admit'](data,entry,context)
            self.assertEqual(record['context'],context);self.assertFalse(record['owner_qualified'])
            key='\n'.join((context['source'],'1','1',p['ROLE'],entry['sha256'],'1000','111',record['input_record_sha256']))
            self.assertEqual(record['cache_key_sha256'],hashlib.sha256(key.encode('ascii')).hexdigest())
            for key,value in (('role','TOOL'),('name','cdb.exe'),('bytes',True),('bytes',len(data)-1),
                              ('version','wrong'),('sha256','0'*64)):
                bad=entry|{key:value};self.reject(lambda:p['admit'](data,bad,context))
            for bad in (context|{'source':'x'},context|{'run':1},context|{'attempt':'0'},context|{'extra':'x'}):
                self.reject(lambda:p['admit'](data,entry,bad))
        finally:profiles.clear();profiles.update(original)
    def test_versioned_frame_and_unchanged_legacy_eof(self):
        data,entry=provider_fixture();context={'source':'a'*40,'run':'1','attempt':'1'}
        entries=[{'name':name,'bytes':len(data),'sha256':entry['sha256']} for name in sorted(pe.ADMITTED_NAMES)]
        old=json.dumps({'entries':entries}).encode()+b'\n'+(data+b'\nFONT_RESOURCE_PE_END\n')*3
        # Direct old behavior stays exact, including its mandatory EOF.
        self.assertEqual(len(list(legacy.inventory(io.BytesIO(old),pe))),3)
        self.reject(lambda:list(legacy.inventory(io.BytesIO(old+b'x'),pe)))
        import types
        provider=types.SimpleNamespace(**p)
        profiles=p['admit'].__globals__['PROFILES'];original=dict(profiles)
        def frame(old=old,entry=entry,payload=data,tail=b'\nFONT_UCRT_CODE_END_V1\n\0\n',extra=b'',version=1):
            return json.dumps({'version':version,'legacy_bytes':len(old),'context':context}).encode()+b'\n'+old+json.dumps(entry).encode()+b'\n'+payload+tail+extra
        def decode(value):return list(e['envelope'](io.BytesIO(value),legacy,pe,provider))
        try:
            profiles[entry['sha256']]=('SYNTHETIC_ONLY',0x1000,0x100000)
            rows=decode(frame());self.assertEqual(len(rows),4);self.assertEqual(rows[3]['record'],'PROVIDER_CACHE')
            for value in (frame(old=old+b'x'),frame(old=old[:-22]),frame(extra=b'x'),frame(tail=b''),
                frame(tail=b'\nFONT_RESOURCE_PE_END\n'),frame(payload=data[:-1]),frame(version=2),
                frame(entry=entry|{'role':'TOOL'}),frame(entry=entry|{'bytes':-1}),
                frame(entry=entry|{'bytes':p['MAX_BYTES']+1}),frame(entry=entry|{'sha256':'0'*64}),
                frame(entry=entry|{'offset':1}),frame(entry=entry|{'name':'C:\\arbitrary.dll'}),
                frame(extra=frame()),frame()[:-len(data)-24],b' '*16385,
                frame().replace(b'"version": 1',b'"version": 1, "version": 1',1)):
                self.reject(lambda:decode(value))
        finally:profiles.clear();profiles.update(original)
    def test_old_codecs_immutable(self):
        import subprocess
        base='a8e79c27639200c3a3ee9162fb053a22727df3f6'
        for name in ('resource-import-inventory.py','resource-pe-imports.py'):
            self.assertEqual((ROOT/name).read_bytes(),subprocess.check_output(['git','show',base+':tools/fonts/'+name]))

if __name__=='__main__':unittest.main()
