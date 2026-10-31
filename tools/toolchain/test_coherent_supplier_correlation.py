"""Pure correlation controls on a preserved historical supplier receipt."""
import copy,json
from pathlib import Path
import coherent_supplier_correlation as c

def main():
 root=Path(__file__).resolve().parents[2]
 a=json.loads((root/'tests/net-tcp-priority/minimum-receipt-411350.json').read_text(encoding='utf-8'))
 result=c.compare(a,copy.deepcopy(a));assert result['underlying_clang_equality']=='UNKNOWN_NOT_IN_FULL_RECEIPT'
 changes={
  'role':lambda d:d.__setitem__('role','foreign'),
  'pin':lambda d:d['pin'].__setitem__('role','foreign'),
  'source_files':lambda d:d['source_files'].__setitem__(next(iter(d['source_files'])),'0'*64),
  'compiler_archive_sha256':lambda d:d.__setitem__('compiler_archive_sha256','0'*64),
  'files':lambda d:d['files'].__setitem__('lib/libpthread.a','0'*64),
  'public_headers':lambda d:d['public_headers'].__setitem__('pthread.h','0'*64),
  'private_record_layout':lambda d:d.__setitem__('private_record_layout','foreign'),
  'original_profile':lambda d:d['original_profile']['sdk'].__setitem__('WINVER',0),
  'compiler_driver_sha256':lambda d:d.__setitem__('compiler_driver_sha256','0'*64),
  'cxx_driver_sha256':lambda d:d.__setitem__('cxx_driver_sha256','0'*64),
  'dll_exports':lambda d:d.__setitem__('dll_exports',[]),
  'dll_imports':lambda d:d.__setitem__('dll_imports',''),
  'dll_symbol_objects':lambda d:[v.__setitem__('sha256','0'*64) for v in d['dll_symbol_objects'].values()],
 }
 for name,mutate in changes.items():
  b=copy.deepcopy(a);mutate(b)
  try:c.compare(a,b)
  except (AssertionError,KeyError,ValueError,TypeError):pass
  else:raise AssertionError('supplier mismatch accepted: '+name)
 both=copy.deepcopy(a);both['files']['lib/libpthread.a']='0'*64
 try:c.compare(both,copy.deepcopy(both))
 except AssertionError:pass
 else:raise AssertionError('matched broken aliases accepted')
 print('COHERENT_SUPPLIER_CORRELATION_PURE_PASS mismatches='+str(len(changes))+' alias=1 actual_binary_equality=false')

if __name__=='__main__':main()
