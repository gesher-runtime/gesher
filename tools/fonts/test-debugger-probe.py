"""Light pure metadata controls; all COFF bytes below are SYNTHETIC memory.
No binary is compiled, downloaded, written or executed by these tests.
"""
import importlib.util,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
spec=importlib.util.spec_from_file_location('font_debugger_probe',Path(__file__).with_name('debugger-probe.py'))
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
root='/home/runner/work/gesher/gesher'
compiler=root+'/toolchain/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/bin/x86_64-w64-mingw32-clang++'
source=root+'/'+p.SOURCE;obj=root+'/preflight-probe/probe.obj';exe=root+'/preflight-probe/probe.exe';mapping=root+'/preflight-probe/probe.map'
commands=p.commands(compiler,source,obj,exe,mapping)
assert len(commands)==2 and len(commands[0])==13 and commands[0][10]==source and commands[0][12]==obj
assert commands[0].count('-c')==commands[0].count('-o')==1 and all(v in commands[0] for v in ('-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti'))
assert commands[1]==[compiler,obj,'-static','-s','-Wl,-Map,'+mapping,'-o',exe]

def coff(*,aux=0,long_name=False):
 strings=struct.pack('<I',9)+b'main\0' if long_name else struct.pack('<I',4)
 name=bytes(4)+struct.pack('<I',4) if long_name else b'main\0\0\0\0'
 symbol=name+struct.pack('<IhHBB',0,1,0x20,2,aux)
 header=struct.pack('<HHIIIHH',0x8664,1,0,60,1+aux,0,0)
 return bytearray(header+bytes(40)+symbol+bytes(18*aux)+strings)

for value in (coff(),coff(aux=1),coff(long_name=True)):
 p.main_symbol(bytes(value))
negative=0

def reject(value):
 global negative
 try:p.main_symbol(bytes(value))
 except (AssertionError,IndexError):negative+=1
 else:raise AssertionError('Malformed synthetic COFF symbol accepted')

for size in (0,1,19,20,59,60,77,78,81):reject(coff()[:size])
for offset,replacement in (
 (0,b'\x3e\x00'),(2,b'\x00\x00'),(8,struct.pack('<I',19)),
 (8,struct.pack('<I',0xffffffff)),(12,struct.pack('<I',0)),
 (12,struct.pack('<I',0xffffffff)),(16,b'\x01\x00'),
 (60,b'fake'),(72,b'\x00\x00'),(72,b'\x02\x00'),
 (74,b'\x00\x00'),(76,b'\x03'),(77,b'\x01'),
 (78,struct.pack('<I',3)),(78,struct.pack('<I',1000))):
 value=coff();value[offset:offset+len(replacement)]=replacement;reject(value)
value=coff(long_name=True);value[64:68]=struct.pack('<I',1);reject(value)
value=coff(long_name=True);value[-1]=ord('x');reject(value)
assert negative==26
print('FONT_DEBUGGER_PROBE_METADATA_PASS fixed_compile_link_argv=1 synthetic_COFF_positives=3 negatives=26 native_processes_started=0')
