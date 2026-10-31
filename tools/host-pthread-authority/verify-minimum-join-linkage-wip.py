"""WIP selected diagnostic archive/PE bindings. No runtime authorization.

Run only inside CI on current-attempt files after an immutable source gate and
artifact receipt. A future caller must independently attest the producer role.
"""
import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import shlex
import sys

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import minimum_bundle as prior

REQUIRED={'pthread_create','pthread_join','pthread_cancel','pthread_gethandle',
          'gesher_minimum_join_diag_configure','gesher_minimum_join_diag_snapshot'}

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def verify_supplier(root,context):
    receipt=prior.receipt
    sdk=(root/'sdk-macros.txt').read_text(encoding='utf-8')
    supplier=(root/'supplier-macros.txt').read_text(encoding='utf-8')
    diagnostic='#define GESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC 1'
    assert supplier.splitlines().count(diagnostic)==1
    supplier='\n'.join(line for line in supplier.splitlines() if line!=diagnostic)+'\n'
    assert receipt.actual_macros(sdk)==receipt.actual_macros(supplier)
    for name in ('sdk-machine.txt','supplier-machine.txt','dll-machine.txt'):
        assert 'IMAGE_FILE_MACHINE_AMD64' in (root/name).read_text(encoding='utf-8')
    baseline=receipt.exports((root/'publisher-dll-exports.txt').read_text(encoding='utf-8'))
    actual=receipt.exports((root/'dll-exports.txt').read_text(encoding='utf-8'))
    extra={'gesher_minimum_join_diag_configure','gesher_minimum_join_diag_snapshot'}
    assert not (set(baseline)&extra) and set(actual)==set(baseline)|extra
    assert receipt.imports((root/'dll-imports.txt').read_text(encoding='utf-8'))==receipt.imports((root/'publisher-dll-imports.txt').read_text(encoding='utf-8'))
    headers=json.loads((root/'publisher-headers.json').read_text(encoding='utf-8'))
    assert headers==json.loads((root/'postbuild-headers.json').read_text(encoding='utf-8'))
    assert set(headers)=={'pthread.h','pthread_compat.h','pthread_signal.h','pthread_time.h','pthread_unistd.h','sched.h','semaphore.h'}
    assert headers['pthread.h']==receipt.PIN['source_sha256']['mingw-w64-libraries/winpthreads/include/pthread.h']
    log=(root/'supplier-build.log').read_text(encoding='utf-8')
    layouts=re.findall(r'^\s*0 \| struct _pthread_v\n(.*?\[sizeof=\d+, align=\d+\])',log,re.M|re.S)
    assert layouts;receipt.validate_layout(layouts[-1])
    origin=json.loads((root/'dll-source-observations.json').read_text(encoding='utf-8'))
    assert set(origin)=={'source','run','attempt','role','build','objects','compile_commands','qualification'}
    assert {k:origin[k] for k in ('source','run','attempt')}==context
    assert origin['role']=='join-wait-diagnostic-only-v1' and origin['qualification'] is False
    build=origin['build'];assert build.startswith('/') and posixpath.normpath(build)==build
    assert build.endswith('/join-runtime-'+context['run']+'-'+context['attempt']+'/build')
    names=receipt.CORE+('gesher_minimum_join_diag_configure','gesher_minimum_join_diag_snapshot')
    definitions=receipt.map_symbols((root/'supplier-dll.map').read_text(encoding='utf-8'),names)
    objects=origin['objects'];assert set(objects)==set(names)
    paths=set();obj_hash=digest(root/'dll-source-thread.o')
    reading=set(re.findall(r'\bReading ([^\r\n]+\.(?:o|obj))\r?$',log+'\n'+(root/'supplier-install.log').read_text(encoding='utf-8'),re.M))
    for name,(owner,rva) in definitions.items():
        row=objects[name];assert set(row)=={'input','rva','actual_reading_path','sha256'}
        assert row['input']==owner and row['rva']==rva and row['sha256']==obj_hash
        path=row['actual_reading_path'];assert path.startswith(build+'/') and path.endswith('thread.o') and posixpath.normpath(path)==path
        candidates={posixpath.normpath(p if p.startswith('/') else posixpath.join(build,p)) for p in reading if posixpath.basename(p)==posixpath.basename(owner)}
        assert candidates=={path};paths.add(path)
    assert len(paths)==1 and origin['compile_commands']
    compiled=set()
    for command in origin['compile_commands']:
        assert command in log and '-DDLL_EXPORT' in command and '-DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC' in command
        argv=shlex.split(command);assert '-c' in argv and '-o' in argv
        src=argv[argv.index('-c')+1];out=argv[argv.index('-o')+1]
        src=posixpath.normpath(src if src.startswith('/') else posixpath.join(build,src))
        out=posixpath.normpath(out if out.startswith('/') else posixpath.join(build,out))
        assert src==build[:-len('/build')]+'/source/mingw-w64-libraries/winpthreads/src/thread.c';compiled.add(out)
    assert paths<=compiled
    return {'private_layout':'480-validated','headers':headers,'dll_source_sha256':obj_hash,'dll_exports':actual,'diagnostic_only':True}

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
    supplier_proof=verify_supplier(root,context)
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
    return {'context':context,'role':'join-wait-diagnostic-only-v1','supplier':supplier_proof,'bindings':results,
            'target_wait_proved':False,'product_qualification':False,'sealed':False}

if __name__=='__main__':
    assert os.environ.get('GITHUB_ACTIONS')=='true', 'CI only'
    assert os.environ.get('GESHER_MINIMUM_JOIN_RUNTIME_AUTHORIZED_SOURCE')==os.environ.get('GITHUB_SHA')
    value=verify(Path(sys.argv[1]),{'source':os.environ['GITHUB_SHA'],'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT']})
    print('JOIN_RUNTIME_UNSEALED_LINK_PROOF '+json.dumps(value,sort_keys=True),flush=True)
