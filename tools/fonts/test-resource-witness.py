"""Synthetic in-memory images/metadata only; never a real PE/tool operation."""
import importlib.util
from pathlib import Path
import struct
import unittest
import json
import hashlib
import io
import contextlib
from unittest.mock import patch

def load(name):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
w=load('resource-witness');p=load('resource-code-provider')

def image():
    data=bytearray(0x2400);pe=0x80;opt=pe+24;table=opt+240
    data[:2]=b'MZ';struct.pack_into('<I',data,60,pe);data[pe:pe+4]=b'PE\0\0'
    struct.pack_into('<HH',data,pe+4,0x8664,2);struct.pack_into('<H',data,pe+20,240)
    struct.pack_into('<H',data,opt,0x20b);struct.pack_into('<II',data,opt+56,0x100000,0x400)
    struct.pack_into('<I',data,opt+108,16);struct.pack_into('<II',data,opt+112+24,0x3000,24)
    for i,(rva,raw,flags)in enumerate(((0x1000,0x400,0x60000020),(0x3000,0x1400,0xc0000040))):
        struct.pack_into('<IIII',data,table+i*40+8,0x1000,rva,0x1000,raw)
        struct.pack_into('<I',data,table+i*40+36,flags)
    code=bytearray.fromhex('48895c2408574883ec20488bda488bf9488d0d89c80f00ff15e3080c0090488bd3488bcfe8230000008bd8488d0d6ec80f00ff15e0080c008bc3488b5c24304883c4205fc3')
    struct.pack_into('<i',code,19,0x3200-(0x1000+23));struct.pack_into('<i',code,46,0x3200-(0x1000+50))
    data[0x400:0x400+len(code)]=code
    struct.pack_into('<III',data,0x1400,0x1000,0x1045,0x3100)
    struct.pack_into('<III',data,0x140c,0x104c,0x1100,0x3100)
    data[0x1500:0x150c]=bytes.fromhex('010a04000a3406000a320670')
    return data

def packet(data):
    cache=p.window(bytes(data),0x1000)
    cache['function']=w.runtime_function(bytes(data),cache['section_facts'],cache['exception_directory'],0x1000)
    cache['unwind']=w.unwind_prologue(bytes(data),cache['section_facts'],cache['function'],bytes.fromhex(cache['cache_hex']))
    cache['basis']=w.controlflow_basis(cache);return cache

class Witness(unittest.TestCase):
    def reject(self,fn):
        with self.assertRaises((ValueError,KeyError,TypeError)):fn()
    def test_full_entry_unwind_instruction_controlflow(self):
        value=packet(image());self.assertEqual(value['basis']['candidate_rva'],0x3200)
        self.assertEqual(value['basis']['writer_return_rva'],0x101d)
        self.assertEqual(value['basis']['handler_return_rva'],0x1029)
        self.assertFalse(value['basis']['owner_observed'])
        self.assertFalse(value['basis']['runtime_unwind_integrity_attested'])
    def test_exception_entry_unwind_negative_matrix(self):
        changes=[(0x80+24+112+24,'I',0),(0x80+24+116+24,'I',13),
                 (0x1400,'I',0x1001),(0x1404,'I',0x1029),(0x1404,'I',0x1061),
                 (0x1408,'I',0x3101),(0x1500,'B',2),(0x1500,'B',0x21),
                 (0x1501,'B',9),(0x1502,'B',33),(0x1503,'B',1),
                 (0x1505,'B',0x31),(0x1506,'B',11),(0x1508,'B',0x04),
                 (0x150a,'H',7)]
        for at,fmt,value in changes:
            with self.subTest(at=at,value=value):
                data=image();struct.pack_into('<'+fmt,data,at,value);self.reject(lambda:packet(data))
    def test_all_primary_instruction_mutations_reject(self):
        for at in (0,5,6,10,13,16,23,29,30,33,36,41,43,50,56,58,63,67,68):
            with self.subTest(offset=at):
                data=image();data[0x400+at]^=1;self.reject(lambda:packet(data))
    def test_wrong_object_release_and_bss_bounds(self):
        data=image();data[0x400+46]^=8;self.reject(lambda:packet(data))
        cache=packet(image())
        for target in (0,0x1000,0x3201,0x3fe0,2**32):
            self.reject(lambda:w.virtual_object_bounds(cache['section_facts'],target,0x100000))
        # Mutable object need not be backed by disk bytes.
        sections=[{'rva':0x3000,'extent':0x1000,'raw_size':0,'raw':0,'flags':0x80000000}]
        self.assertFalse(w.virtual_object_bounds(sections,0x3200,0x100000)['file_backed_object_required'])
        self.reject(lambda:w.virtual_object_bounds(sections*2,0x3200,0x100000))
    def test_duplicate_overlapping_runtime_entries(self):
        data=image();struct.pack_into('<I',data,0x80+24+116+24,24)
        data[0x140c:0x1418]=data[0x1400:0x140c];self.reject(lambda:packet(data))
    def test_mismatch_lossless_bounded_stderr_evidence(self):
        # Synthetic wrong save offset, NOT an observed current-profile layout.
        data=image();struct.pack_into('<H',data,0x1506,7)
        stdout=io.StringIO()
        with contextlib.redirect_stdout(stdout), self.assertRaises(ValueError)as caught:
            packet(data)
        self.assertEqual(stdout.getvalue(),'')
        reason,suffix=str(caught.exception).split('\nFONT_RESOURCE_UNWIND_MISMATCH ')
        self.assertEqual(reason,'UNAVAILABLE: prologue/unwind register and stack coherence')
        self.assertLessEqual(len(suffix.encode('ascii')),4096)
        value=json.loads(suffix);raw=bytes(data[0x1500:0x150c])
        self.assertEqual(value['schema'],'FONT_RESOURCE_UNWIND_MISMATCH_V1')
        self.assertEqual(value['selected_entry'],{'begin':0x1000,'end':0x1045,'unwind_rva':0x3100,'entry_hex':bytes(data[0x1400:0x140c]).hex()})
        self.assertEqual(bytes.fromhex(value['header_hex']),raw[:4])
        self.assertEqual(bytes.fromhex(value['raw_hex']),raw)
        self.assertEqual(value['raw_sha256'],hashlib.sha256(raw).hexdigest())
        self.assertEqual([value[k]for k in ('version','flags','count','prolog','frame')],[1,0,4,10,0])
        self.assertEqual(value['expected_operations'],[[10,4,3,48],[10,2,3,32],[6,0,7,0]])
        self.assertEqual(value['received_operations'],[[10,4,3,56],[10,2,3,32],[6,0,7,0]])
        self.assertEqual(bytes.fromhex(value['prologue_hex']),bytes(data[0x400:0x40a]))
    def test_diagnostic_failures_keep_original_rejection(self):
        data=image();struct.pack_into('<H',data,0x1506,7)
        for target,value in [('UNWIND_DIAGNOSTIC_CAP',1),('json.dumps',lambda *a,**k: (_ for _ in ()).throw(RuntimeError('internal')))]:
            control=patch.object(w,'UNWIND_DIAGNOSTIC_CAP',value)if target=='UNWIND_DIAGNOSTIC_CAP'else patch.object(w.json,'dumps',value)
            with self.subTest(target=target),control:
                with self.assertRaisesRegex(ValueError,'^UNAVAILABLE: prologue/unwind register and stack coherence$'):
                    packet(data)
        for raw,ops in [(b'x'*257,[]),(b'1234',[(1,2,3,4)]*33),(b'1234',[[1,2,3,4]]),(b'1234',[(1,2,3,2**32)])]:
            self.assertEqual(w._unwind_mismatch_evidence({},raw,b'0'*10,ops),'')
        maximum=w._unwind_mismatch_evidence({'begin':2**32-1,'end':2**32-1,'unwind_rva':2**32-1,'entry_hex':'ff'*12},b'\xff'*256,b'0'*10,[(2**32-1,)*4]*32)
        self.assertTrue(maximum)
        self.assertLessEqual(len(maximum.encode('ascii')),4096)
    def test_valid_result_exact_and_no_diagnostic_or_reordered_failure(self):
        baseline=packet(image())
        with patch.object(w,'_unwind_mismatch_evidence',side_effect=AssertionError('valid must not format')):
            self.assertEqual(json.dumps(packet(image()),sort_keys=True),json.dumps(baseline,sort_keys=True))
        for at,value in [(0x1500,2),(0x1505,0x31),(0x1504,11)]:
            data=image();data[at]=value
            with patch.object(w,'_unwind_mismatch_evidence',side_effect=AssertionError('earlier guard must reject first')):
                with self.assertRaises(ValueError):packet(data)
        data=image()[:0x150a]
        with patch.object(w,'_unwind_mismatch_evidence',side_effect=AssertionError('truncation first')):
            with self.assertRaises(ValueError):packet(data)
    def test_actual_saved_metadata_and_fixed_frame_correction(self):
        # Saved TEXT diagnostics, not local PE reads. d40e/run41198747288/a1.
        fixtures=[(124530422116,'feafcc3e4cdd35eff38da982e39b639548122e62010f617e76d57132909056c0',
                   'd8c103001dc2030088ce1100','190a04000a3406000a32067010170b00','678373fdb3eb3c07aeb2efee86558a3aca49dd0695cb51bdfc2da57577748f3c'),
                  (124530422148,'b302ae62c709893fb175be0bcc76c066ff86797be0c143c7c5e57a4a16743c18',
                   'b4840b00f9840b00b4e50e00','190a04000a3406000a32067070570400','c4efbe6192b33830570a209b4cf339c8b9aa396f4c2880c08a5b568def8de45b')]
        source='d40e5398f22c8edaa85c36543b467f2ad1315ffb'
        self.assertEqual(len(source),40)
        for job,raw_sha,entry_hex,unwind_hex,digest in fixtures:
            with self.subTest(job=job,raw_sha=raw_sha):
                begin,end,at=struct.unpack('<III',bytes.fromhex(entry_hex));raw=bytes.fromhex(unwind_hex)
                self.assertEqual(hashlib.sha256(raw).hexdigest(),digest)
                handler=struct.unpack_from('<I',raw,12)[0]
                sections=[{'rva':at,'raw':0,'raw_size':16,'flags':0},
                          {'rva':handler,'raw':0,'raw_size':1,'flags':0x20000000}]
                result=w.unwind_prologue(raw,sections,{'begin':begin,'end':end,'unwind_rva':at,'entry_hex':entry_hex},bytes.fromhex('48895c2408574883ec20'))
                self.assertEqual(result['operations'],[(10,4,3,48),(10,2,3,32),(6,0,7,0)])
                self.assertEqual(result['scope'],'POST_PROLOGUE_COMMON_BODY')
                self.assertIs(result['partial_prologue_qualified'],False)
                self.assertIs(result['runtime_unwind_integrity'],False)
        # Both inverses read SAVE from FIXED F, including after virtual SP moves.
        self.assertEqual(w._fixed_body_state([(10,4,3,48),(10,2,3,32),(6,0,7,0)]),
                         w._fixed_body_state([(10,2,3,32),(6,0,7,0),(5,4,3,48)]))
        data=image();data[0x1500:0x150c]=bytes.fromhex('010a04000a32067005340600')
        self.reject(lambda:packet(data)) # Old list is NOT actual raw encoding.
    def test_scope_missing_or_wrong_type_and_body_bounds(self):
        for key in ('scope','partial_prologue_qualified','runtime_unwind_integrity'):
            for value in (None,0,'false',True):
                c=packet(image());c['unwind'][key]=value
                self.reject(lambda:w.controlflow_basis(c))
            c=packet(image());del c['unwind'][key];self.reject(lambda:w.controlflow_basis(c))
        c=packet(image());self.assertEqual(c['basis']['body_after_rva'],0x100a)
        self.assertEqual(c['basis']['body_before_rva'],0x103a)

def version_block(key,value=b'',kind=0,children=(),units=None):
    raw=bytearray(b'\0'*6+key.encode('utf-16-le')+b'\0\0')
    raw.extend(b'\0'*((-len(raw))%4));raw.extend(value)
    for child in children:
        raw.extend(b'\0'*((-len(raw))%4));raw.extend(child)
    struct.pack_into('<HHH',raw,0,len(raw),len(value)//2 if kind else len(value),kind)
    if units is not None:struct.pack_into('<H',raw,2,units)
    return bytes(raw)

def version_root(strings=None,translations=None,fixed=None,extra=()):
    if fixed is None:fixed=[0xfeef04bd,0x10000,0xa0000,0x4f7c167c,0xa0000,0x4f7c167c,63,0,0x40004,2,0,0,0]
    groups=[]
    if strings is not None:
        tables=[]
        for lang,items in strings:
            tables.append(version_block(lang,kind=1,children=[version_block(k,(v+'\0').encode('utf-16-le'),1)for k,v in items]))
        groups.append(version_block('StringFileInfo',kind=1,children=tables))
    if translations is not None:
        groups.append(version_block('VarFileInfo',kind=1,children=[version_block('Translation',b''.join(struct.pack('<HH',*v)for v in translations))]))
    return version_block('VS_VERSION_INFO',struct.pack('<13I',*fixed),children=groups+list(extra))

def disk_image(payload=None,records=None):
    if payload is None:payload=version_root([('040904B0',[('FileVersion','10.0.20348.5622'),('ProductVersion','localized distinct')])],[(0x409,1200)])
    if records is None:records=[(0x409,payload)]
    data=image();opt=0x80+24
    # All bytes constructed in memory; these are NOT NTDLL/profile bytes.
    data[0x1400:0x2400]=b'\0'*0x1000
    struct.pack_into('<II',data,opt+112+16,0x3000,0x1000)
    def node(at,rows):
        struct.pack_into('<HH',data,0x1400+at+12,0,len(rows))
        for i,row in enumerate(rows):struct.pack_into('<II',data,0x1400+at+16+i*8,*row)
    node(0,[(16,0x80000018)]);node(24,[(1,0x80000030)])
    node(48,[(lang,96+i*16)for i,(lang,_)in enumerate(records)])
    cursor=0x200
    for i,(lang,body)in enumerate(records):
        struct.pack_into('<IIII',data,0x1400+96+i*16,0x3000+cursor,len(body),1200,0)
        data[0x1400+cursor:0x1400+cursor+len(body)]=body;cursor=(cursor+len(body)+3)&~3
    return bytes(data)

def disk_entry(data,context=None,pid=123):
    return {'schema':1,'role':w.DISK_ROLE,'name':'ntdll.dll','bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
            'version':'SYNTHETIC_ONLY','path':r'C:\Windows\System32\ntdll.dll','signature_status':'Valid',
            'signer':'CN=Synthetic, O=Microsoft Corporation','signer_thumbprint':'A'*40,
            'lease':'RETAINED_READ_SHARE_READ','context':context or {'source':'a'*40,'run':'1','attempt':'1'},'imports_pid':pid}

class DiskMetadata(unittest.TestCase):
    def reject(self,fn):
        with self.assertRaises((ValueError,KeyError,TypeError,struct.error)):fn()
    def test_effective_walker_all_categories_and_languages(self):
        payload=version_root([('040904B0',[('FileVersion','10.0.20348.5622')]),('040D04B0',[('FileVersion','\u05d0\u05d1')])],[(0x409,1200),(0x40d,1200)])
        result=w.disk_version_metadata(disk_image(records=[(0x409,payload),(0x40d,payload)]))
        self.assertEqual(len(result['records']),2)
        v=result['records'][0]['version']
        self.assertEqual(v['file_version_components'],[10,0,20348,5756])
        self.assertEqual(v['string_tables'][0]['strings']['FileVersion'],'10.0.20348.5622')
        self.assertEqual(v['string_tables'][1]['strings']['FileVersion'],'\u05d0\u05d1')
        self.assertEqual(v['translations'],[[0x409,1200],[0x40d,1200]])
        self.assertEqual(v['raw_sha256'],hashlib.sha256(payload).hexdigest())
        self.assertFalse(result['identity_predicate_repaired']);self.assertFalse(result['loaded_memory_identity_attested'])
        self.assertEqual(result['headers']['coff_timestamp_raw'],0)
        self.assertFalse(result['headers']['coff_timestamp_is_wall_clock_attested'])
    def test_disk_pe_header_spans_and_overlaps(self):
        for at,fmt,val in [(0,'H',0),(60,'I',2**32-1),(0x84,'H',0x14c),(0x86,'H',97),
             (0x94,'H',111),(0x98,'H',0x10b),(0x98+108,'I',17),(0x98+60,'I',1),
             (0x98+112+16,'I',0xfffffff0),(0x98+116+16,'I',0xffffffff),
             (0x188+40+12,'I',0x1000),(0x188+40+20,'I',0x400),
             (0x188+40+16,'I',1),(0x188+40+12,'I',0xfffffff0)]:
            with self.subTest(at=at,val=val):
                data=bytearray(disk_image());struct.pack_into('<'+fmt,data,at,val)
                self.reject(lambda:w.disk_version_metadata(bytes(data)))
        for size in (0,63,127,0x1d7,0x23ff):self.reject(lambda:w.disk_version_metadata(disk_image()[:size]))
        with patch.object(w,'DISK_MAX',len(disk_image())-1):self.reject(lambda:w.disk_version_metadata(disk_image()))
    def test_resource_exact_hierarchy_caps_alias_overflow(self):
        for offset,fmt,val in [(12,'H',1),(14,'H',65),(16,'I',15),(16,'I',0x80000000),
             (20,'I',24),(20,'I',0x80000000),(20,'I',0x80000fff),
             (44,'I',0x80000018),(68,'I',0x80000030),(68,'I',0xfffffff0),
             (96,'I',0xfffffff0),(96,'I',0x3201),(100,'I',65536),(100,'I',0),(108,'I',1)]:
            with self.subTest(offset=offset):
                data=bytearray(disk_image());struct.pack_into('<'+fmt,data,0x1400+offset,val)
                self.reject(lambda:w.disk_version_metadata(bytes(data)))
        # Unique language hierarchy and payload intervals are mandatory.
        p1=version_root();p2=version_root()
        data=bytearray(disk_image(records=[(0x409,p1),(0x40d,p2)]))
        for offset,value in [(72,0x409),(76,96),(112,0x3200),(96,0x3000)]:
            bad=bytearray(data);struct.pack_into('<I',bad,0x1400+offset,value)
            self.reject(lambda:w.disk_version_metadata(bytes(bad)))
        fixed=[0xfeef04bd,0x10000,1,2,3,4,63,0,0,0,0,0,0]
        self.reject(lambda:w.disk_version_metadata(disk_image(records=[(0x409,p1),(0x40d,version_root(fixed=fixed))])))
        # Nine leaves exceed8 regardless of a small sum; each WORD<65536 and
        # eight leaves imply aggregate<512KiB (the explicit aggregate gate stays).
        self.reject(lambda:w.disk_version_metadata(disk_image(records=[(i,p1)for i in range(9)])))
    def test_version_units_alignment_unknown_duplicate_children(self):
        good=version_root([('040904B0',[('FileVersion','x')])],[(0x409,1200)])
        for offset,fmt,val in [(0,'H',len(good)-1),(2,'H',51),(4,'H',1),(6,'H',ord('X')),
                               (38,'B',1),(40,'I',0),(44,'I',0x20000)]:
            bad=bytearray(good);struct.pack_into('<'+fmt,bad,offset,val)
            self.reject(lambda:w.version_payload(bytes(bad)))
        self.reject(lambda:w.version_payload(good+b'\0'))
        self.reject(lambda:w.version_payload(b'\0'*65536))
        for child in [version_block('Unknown',kind=1),version_block('StringFileInfo',kind=0),
                      version_block('VarFileInfo',kind=1,children=[]),version_block('VarFileInfo',kind=1,children=[version_block('Translation',b'abc')])]:
            self.reject(lambda:w.version_payload(version_root(extra=[child])))
        self.reject(lambda:w.version_payload(version_root([('040904B0',[('X','x'),('X','y')])])))
        self.reject(lambda:w.version_payload(version_root([('040904B0',[]),('040904b0',[])])))
        self.reject(lambda:w.version_payload(version_root([('garbage!',[])])))
        self.reject(lambda:w.version_payload(version_root(translations=[(1,2),(1,2)])))
        string=version_block('X',b'\x00\xd8\0\0',1)
        group=version_block('StringFileInfo',kind=1,children=[version_block('040904B0',kind=1,children=[string])])
        self.reject(lambda:w.version_payload(version_root(extra=[group])))
        for value,units in [(b'a\0',2),(b'a\0b\0',1),(b'\0\0a\0',2)]:
            bad=version_block('X',value,1,units=units)
            group=version_block('StringFileInfo',kind=1,children=[version_block('040904B0',kind=1,children=[bad])])
            self.reject(lambda:w.version_payload(version_root(extra=[group])))
    def test_frame_full_hash_signature_lease_context_pid_binding(self):
        data=disk_image();entry=disk_entry(data);context=entry['context'];profiles=dict(w.DISK_PROFILES)
        self.reject(lambda:w.admit_disk_metadata(data,entry,context,123)) # Synthetic is never a production profile.
        try:
            w.DISK_PROFILES[entry['sha256']]='SYNTHETIC_ONLY'
            result=w.admit_disk_metadata(data,entry,context,123)
            self.assertEqual(result['profile'],entry);self.assertFalse(result['qualification'])
            self.assertEqual(result['input_record_sha256'],hashlib.sha256(json.dumps(entry,sort_keys=True,separators=(',',':')).encode()).hexdigest())
            for key,value in [('schema',True),('schema',2),('role','SIGNED_SYSTEM32_UCRT_CODE'),('name','ucrtbase.dll'),
                ('bytes',True),('bytes',len(data)-1),('sha256','0'*64),('version','5756'),('context',context|{'run':'2'}),
                ('imports_pid',124),('imports_pid',True),('signature_status','NotSigned'),('signer','O=Other'),
                ('signer_thumbprint','bad'),('lease','CLOSED'),('path',r'C:\Windows\Other\ntdll.dll'),
                ('path',r'C:\Windows\System32\..\System32\ntdll.dll'),('path',r'C:\Windows.\System32\ntdll.dll')]:
                with self.subTest(key=key,value=value):self.reject(lambda:w.admit_disk_metadata(data,entry|{key:value},context,123))
            for ctx in [context|{'extra':'x'},context|{'attempt':'0'},context|{'source':'x'},context|{'run':True}]:
                self.reject(lambda:w.admit_disk_metadata(data,entry,ctx,123))
            self.reject(lambda:w.admit_disk_metadata(data+b'x',entry,context,123))
        finally:w.DISK_PROFILES.clear();w.DISK_PROFILES.update(profiles)
    def test_debug_repro_is_not_a_clock_and_is_file_rva_bound(self):
        data=bytearray(disk_image());opt=0x98
        struct.pack_into('<I',data,0x88,0xdeadbeef)
        struct.pack_into('<II',data,opt+112+6*8,0x30c0,28)
        struct.pack_into('<IIHHIIII',data,0x14c0,0,123,0,0,16,4,0x3140,0x1540)
        data[0x1540:0x1544]=b'REPR'
        result=w.disk_version_metadata(bytes(data))
        self.assertEqual(result['headers']['coff_timestamp_raw'],0xdeadbeef)
        self.assertFalse(result['headers']['coff_timestamp_is_wall_clock_attested'])
        self.assertEqual(result['headers']['repro_records'],[{'bytes':4,'sha256':hashlib.sha256(b'REPR').hexdigest()}])
        for at,value in [(opt+116+6*8,27),(opt+116+6*8,65*28),(0x14c0+16,65537),
                         (0x14c0+20,0xffffffff),(0x14c0+24,0x1544)]:
            bad=bytearray(data);struct.pack_into('<I',bad,at,value);self.reject(lambda:w.disk_version_metadata(bytes(bad)))
    def test_nested_count_text_caps_and_entire_parent_alignment(self):
        self.reject(lambda:w.version_payload(version_root([('040904B0',[('K'+str(i),'x')for i in range(65)])])))
        self.reject(lambda:w.version_payload(version_root([('040904B0',[('X','x'*4096)])])))
        # <=64 entries/node can still exceed the independent aggregate256 nodes.
        self.reject(lambda:w.version_payload(version_root([(f'{i:04X}04B0',[(str(j),'x')for j in range(64)])for i in range(4)])))
        self.reject(lambda:w.version_payload(version_root(extra=[version_block('StringFileInfo',kind=1),version_block('StringFileInfo',kind=1)])))
        good=version_root()
        for at,val in [(6,0xd800),(38,1)]:
            bad=bytearray(good);struct.pack_into('<H',bad,at,val);self.reject(lambda:w.version_payload(bytes(bad)))
        # Every malformed prefix is processed by the real product walker.
        for n in range(len(good)):
            self.reject(lambda:w.version_payload(good[:n]))
        # Distinct fixed categories may differ from localized strings, but two
        # same-language resource records under different version names cannot
        # choose a convenient interpretation.
        data=bytearray(disk_image());struct.pack_into('<H',data,0x1400+24+14,2)
        struct.pack_into('<II',data,0x1400+48,2,0x80000100)
        struct.pack_into('<HH',data,0x1500+12,0,1);struct.pack_into('<II',data,0x1500+16,0x409,96)
        self.reject(lambda:w.disk_version_metadata(bytes(data)))

if __name__=='__main__':unittest.main()
