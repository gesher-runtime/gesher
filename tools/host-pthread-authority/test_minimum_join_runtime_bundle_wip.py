"""Pure synthetic artifact envelope controls; no real native binary is run."""
import copy, importlib.util, json, tempfile
from pathlib import Path
from unittest import mock
p=Path(__file__).with_name('minimum_join_runtime_bundle_wip.py');spec=importlib.util.spec_from_file_location('bundle',p)
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
def reject(fn):
 try:fn()
 except (AssertionError,ValueError,KeyError):return
 raise AssertionError('bad artifact envelope accepted')
def main():
 context={'source':'1'*40,'run':123,'attempt':1,'guard':'example@'+'2'*40,'role':'diagnostic-producer'}
 producer={'context':context,'roles':{'source-authority / verify':11,'diagnostic-producer':12},'qualification':False}
 consumer={'context':dict(context,role='diagnostic-windows'),'roles':{'source-authority / verify':11,'diagnostic-producer':12,'consumer':13},'qualification':False}
 proof={'target_wait_proved':False,'product_qualification':False,'sealed':False,'synthetic':True}
 with tempfile.TemporaryDirectory(prefix='join-envelope-') as d:
  root=Path(d)
  for name in b.PAYLOAD-{'link-proof.json'}:(root/name).write_bytes(('inert '+name).encode())
  with mock.patch.object(b.linkage,'verify',return_value=proof):
   manifest=b.seal(root,producer);assert manifest['target_wait_proved'] is False and b.verify(root,consumer)==manifest
   count=0
   for name in b.PAYLOAD:
    path=root/name;old=path.read_bytes();path.write_bytes(old+b'x');reject(lambda:b.verify(root,consumer));path.write_bytes(old);count+=1
   for key,value in (('source','2'*40),('run',124),('attempt',2),('guard','bad')):
    bad=copy.deepcopy(consumer);bad['context'][key]=value;reject(lambda:b.verify(root,bad));count+=1
   bad=copy.deepcopy(consumer);bad['roles']['diagnostic-producer']=99;reject(lambda:b.verify(root,bad));count+=1
   extra=root/'unexpected';extra.write_text('inert');reject(lambda:b.verify(root,consumer));extra.unlink();count+=1
   print('JOIN_RUNTIME_ENVELOPE_PURE_PASS negatives='+str(count)+' synthetic_only=true native_network=0')
if __name__=='__main__':main()
