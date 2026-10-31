"""Pure glue negatives; synthetic bytes are not native executables."""
import hashlib, importlib.util, json, tempfile
from pathlib import Path
from unittest import mock

path=Path(__file__).with_name('verify-minimum-join-linkage-wip.py')
spec=importlib.util.spec_from_file_location('join_link_wip',path)
v=importlib.util.module_from_spec(spec);spec.loader.exec_module(v)

def reject(fn):
    try:fn()
    except (AssertionError,ValueError,KeyError):return
    raise AssertionError('bad proof accepted')

def main():
    context={'source':'1'*40,'run':'123','attempt':'1'}
    with tempfile.TemporaryDirectory(prefix='join-link-glue-') as d:
        root=Path(d); archives={}
        for name in ('libwinpthread.a','libwinpthread.dll.a'):
            data=('synthetic '+name).encode();(root/name).write_bytes(data)
            archives[name]={'path':'/runner/toolchain/x86_64-w64-mingw32/lib/'+name,'sha256':hashlib.sha256(data).hexdigest()}
        obs=dict(context,role='join-wait-diagnostic-only-v1',archives=archives,sealed=False)
        manifest=root/'producer-archive-observations.json';manifest.write_text(json.dumps(obs),encoding='utf-8')
        (root/'candidate-undefined.txt').write_text(''.join(' U '+x+'\n' for x in sorted(v.REQUIRED)),encoding='utf-8')
        for mode in ('static','dynamic'):
            (root/(mode+'.exe')).write_bytes(b'INERT')
            (root/(mode+'-imports.txt')).write_text('IMAGE_FILE_MACHINE_AMD64',encoding='utf-8')
            for suffix in ('-symbols.txt','-link.txt'):(root/(mode+suffix)).write_text('synthetic',encoding='utf-8')
        (root/'static.map').write_text('synthetic',encoding='utf-8');(root/'import-archive-symbols.txt').write_text('synthetic',encoding='utf-8')
        def modules(_):return {'kernel32.dll':[]} if modules.calls.pop(0)=='static' else {'kernel32.dll':[],'libwinpthread-1.dll':[]}
        def check():
            modules.calls=['static','dynamic']
            with mock.patch.object(v.prior,'import_symbols',side_effect=modules), \
                 mock.patch.object(v.prior,'static_bindings',return_value={'synthetic':True}), \
                 mock.patch.object(v.prior,'short_import_members',return_value={}), \
                 mock.patch.object(v.prior,'pe_import_thunks',return_value={}), \
                 mock.patch.object(v.prior,'dynamic_bindings',return_value={'synthetic':True}):
                return v.verify(root,context)
        value=check();assert value['target_wait_proved'] is False and value['product_qualification'] is False and value['sealed'] is False
        count=0
        for field in ('source','run','attempt','role','sealed'):
            changed=dict(obs);changed[field]='BAD';manifest.write_text(json.dumps(changed),encoding='utf-8');reject(check);count+=1
        manifest.write_text(json.dumps(obs),encoding='utf-8')
        for name in archives:
            original=(root/name).read_bytes();(root/name).write_bytes(original+b'x');reject(check);(root/name).write_bytes(original);count+=1
        original=(root/'candidate-undefined.txt').read_text();(root/'candidate-undefined.txt').write_text(original.replace(' U pthread_join\n',''));reject(check);count+=1
        print('JOIN_LINK_GLUE_PURE_PASS negatives='+str(count)+' binary_or_runtime_qualification=false')
if __name__=='__main__':main()
