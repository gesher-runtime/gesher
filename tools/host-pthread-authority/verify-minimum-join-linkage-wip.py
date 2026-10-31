"""WIP selected diagnostic archive/PE bindings. No runtime authorization.

Run only inside CI on current-attempt files after an immutable source gate and
artifact receipt. A future caller must independently attest the producer role.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import sys

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import minimum_bundle as prior

REQUIRED={'pthread_create','pthread_join','pthread_cancel','pthread_gethandle',
          'gesher_minimum_join_diag_configure','gesher_minimum_join_diag_snapshot'}

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def verify(root,context):
    root=Path(root)
    observations=json.loads((root/'producer-archive-observations.json').read_text(encoding='utf-8'))
    assert set(observations)=={'source','run','attempt','role','archives','sealed'}
    assert {k:observations[k] for k in ('source','run','attempt')}==context
    assert observations['role']=='join-wait-diagnostic-only-v1' and observations['sealed'] is False
    archives=observations['archives'];assert set(archives)=={'libwinpthread.a','libwinpthread.dll.a'}
    for name,row in archives.items():
        assert set(row)=={'path','sha256'} and re.fullmatch('[0-9a-f]{64}',row['sha256'])
        assert row['path'].startswith('/') and row['path'].endswith('/x86_64-w64-mingw32/lib/'+name)
        assert digest(root/name)==row['sha256']
    undefined=(root/'candidate-undefined.txt').read_text(encoding='utf-8')
    requested=sorted({name for name in re.findall(r'^\s*U (\S+)\s*$',undefined,re.M)
       if re.fullmatch(r'(?:__imp_)?(?:_?pthread_\w+|__pth_gpointer_locked|gesher_minimum_join_diag_\w+)',name)})
    assert REQUIRED <= {x.removeprefix('__imp_') for x in requested}
    results={}
    for mode,archive in (('static','libwinpthread.a'),('dynamic','libwinpthread.dll.a')):
        exe=root/(mode+'.exe');imports=(root/(mode+'-imports.txt')).read_text(encoding='utf-8')
        symbols=(root/(mode+'-symbols.txt')).read_text(encoding='utf-8')
        link=(root/(mode+'-link.txt')).read_text(encoding='utf-8')
        assert 'IMAGE_FILE_MACHINE_AMD64' in imports
        modules=prior.import_symbols(imports)
        assert ('libwinpthread-1.dll' in modules)==(mode=='dynamic')
        inputs={archive:archives[archive]}
        if mode=='static':
            bindings=prior.static_bindings((root/'static.map').read_text(encoding='utf-8'),link,inputs,requested)
        else:
            assert not re.search(r'\b(?:__dyn_tls_pthread|__xl_f)\s*$',symbols,re.M)
            members=prior.short_import_members((root/archive).read_bytes())
            thunks=prior.pe_import_thunks(exe.read_bytes(),symbols,requested)
            nm=(root/'import-archive-symbols.txt').read_text(encoding='utf-8')
            bindings=prior.dynamic_bindings(link,inputs,imports,requested,members,nm,thunks)
        results[mode]={'executable_sha256':digest(exe),'archive_sha256':archives[archive]['sha256'],
                       'requested':requested,'symbol_bindings':bindings}
    return {'context':context,'role':'join-wait-diagnostic-only-v1','bindings':results,
            'target_wait_proved':False,'product_qualification':False,'sealed':False}

if __name__=='__main__':
    assert os.environ.get('GITHUB_ACTIONS')=='true', 'CI only'
    assert os.environ.get('GESHER_MINIMUM_JOIN_RUNTIME_AUTHORIZED_SOURCE')==os.environ.get('GITHUB_SHA')
    value=verify(Path(sys.argv[1]),{'source':os.environ['GITHUB_SHA'],'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT']})
    print('JOIN_RUNTIME_UNSEALED_LINK_PROOF '+json.dumps(value,sort_keys=True),flush=True)
