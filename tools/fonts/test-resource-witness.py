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
    data[0x1500:0x150c]=bytes.fromhex('010a04000a32067005340600')
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
        data=image();struct.pack_into('<H',data,0x150a,7)
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
        self.assertEqual(value['expected_operations'],[[10,2,3,32],[6,0,7,0],[5,4,3,48]])
        self.assertEqual(value['received_operations'],[[10,2,3,32],[6,0,7,0],[5,4,3,56]])
        self.assertEqual(bytes.fromhex(value['prologue_hex']),bytes(data[0x400:0x40a]))
    def test_diagnostic_failures_keep_original_rejection(self):
        data=image();struct.pack_into('<H',data,0x150a,7)
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
        for at,value in [(0x1500,2),(0x1505,0x31),(0x1506,11)]:
            data=image();data[at]=value
            with patch.object(w,'_unwind_mismatch_evidence',side_effect=AssertionError('earlier guard must reject first')):
                with self.assertRaises(ValueError):packet(data)
        data=image()[:0x150a]
        with patch.object(w,'_unwind_mismatch_evidence',side_effect=AssertionError('truncation first')):
            with self.assertRaises(ValueError):packet(data)

if __name__=='__main__':unittest.main()
