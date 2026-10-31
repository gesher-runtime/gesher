"""Synthetic in-memory images/metadata only; never a real PE/tool operation."""
import importlib.util
from pathlib import Path
import struct
import unittest

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

if __name__=='__main__':unittest.main()
