"""Pure synthetic selected-envelope projections; no executable/artifact creation."""
from pathlib import Path
import copy,hashlib,importlib.util,json,sys
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import selected_supplier as selected

def project(authority):
    """The input is log metadata, never relabeled as an actual selected build."""
    context=dict(GITHUB_SHA='1'*40,GITHUB_RUN_ID='123456',GITHUB_RUN_ATTEMPT='1',GITHUB_REPOSITORY='gesher-runtime/gesher',GITHUB_REPOSITORY_ID='1426153186')
    root='/metadata-only-selected/tcp-selected-supplier/'+selected.NAME
    source='/metadata-only-selected/tcp-selected-supplier/source'
    old_sdk=authority['compiler'].split('InstalledDir: ',1)[1].split('/bin',1)[0]
    old_source=next(iter(authority['dll_symbol_objects'].values()))['actual_reading_path'].split('/build/',1)[0]
    def rewrite(value):
        if isinstance(value,str):return value.replace(old_sdk,root).replace(old_source,source)
        if isinstance(value,list):return [rewrite(x) for x in value]
        if isinstance(value,dict):return {k:rewrite(v) for k,v in value.items()}
        return value
    a=rewrite(copy.deepcopy(authority));a['role']=selected.ROLE;a['pin']=copy.deepcopy(selected.minimum.PIN)
    for k in selected.AUTHORITY_CONTEXT:a[k]=context[k]
    a['original']['context']={k:context[k] for k in selected.AUTHORITY_CONTEXT}
    supplier=dict(context=context,role=selected.ROLE,supplier_selection='selected-minimum',rebuilt_from_reference_source=True,producer_job='compile',patch_sha256=None,ordered_patch_sha256=copy.deepcopy(selected.minimum.PIN['ordered_patch_sha256']),upstream=selected.minimum.PIN['upstream_commit'],compiler_archive=dict(url=selected.URL,sha256=a['compiler_archive_sha256'],bytes=84003044),compiler_root=root,minimum_authority=a,minimum_authority_sha256=selected.authority_digest(a),libraries_before=a['original']['files'],libraries_after=a['files'],public_headers_before=a['public_headers'],public_headers_after=a['public_headers'],dll_exports=selected.minimum.exports(a['dll_exports']),compiler_driver_sha256=a['compiler_driver_sha256'],cxx_driver_sha256=a['cxx_driver_sha256'],compiler_real_sha256='2'*64,underlying_clang_sha256='3'*64,notice='SYNTHETIC_METADATA_ONLY',notice_sha256=hashlib.sha256(b'SYNTHETIC_METADATA_ONLY').hexdigest(),compiler_version=a['compiler'],source_before=copy.deepcopy(selected.minimum.PIN['source_sha256']),source_after=copy.deepcopy(selected.minimum.PIN['output_source_sha256']),symbols=['pthread_create','pthread_join','pthread_self'],logs={'selected-minimum-factory.log':'4'*64},commands=[dict(argv=['bash','/metadata-only-selected/tools/host-pthread-authority/build-minimum-supplier.sh',root,source],cwd='/metadata-only-selected',log='selected-minimum-factory.log')])
    supplier['source_before']['mingw-w64-libraries/winpthreads/COPYING']=supplier['notice_sha256']
    supplier['source_after']['mingw-w64-libraries/winpthreads/COPYING']=supplier['notice_sha256']
    return supplier,context

def check(authority):
    good,ctx=project(authority);selected.check_selected(good,ctx)
    def flip(f):
        bad=copy.deepcopy(good);f(bad);bad['minimum_authority_sha256']=selected.authority_digest(bad['minimum_authority'])
        try:selected.check_selected(bad,ctx)
        except (AssertionError,KeyError,TypeError,IndexError):return
        raise AssertionError('Mutated selected supplier accepted')
    tests=[
      lambda v:v.update(role='gesher-winpthreads-minimum-candidate-v1'),
      lambda v:v.update(supplier_selection='retirement'),
      lambda v:v.update(rebuilt_from_reference_source=False),
      lambda v:v.update(producer_job='minimum_compile'),
      lambda v:v['context'].update(GITHUB_REPOSITORY_ID='1'),
      lambda v:v.update(patch_sha256='0'*64),
      lambda v:v['ordered_patch_sha256'].pop('minimum-noattach-join.patch'),
      lambda v:v.update(compiler_root='/other/canonical/'+selected.NAME),
      lambda v:v.update(compiler_root='/metadata-only-selected/tcp-selected-supplier/../tcp-selected-supplier/'+selected.NAME),
      lambda v:v['compiler_archive'].update(sha256='0'*64),
      lambda v:v['compiler_archive'].update(bytes=1),
      lambda v:v['minimum_authority'].update(GITHUB_SHA='0'*40),
      lambda v:v['minimum_authority'].update(role='gesher-winpthreads-minimum-candidate-v1'),
      lambda v:v['minimum_authority']['pin'].update(role='other'),
      lambda v:v['minimum_authority']['source_files'].update({'mingw-w64-libraries/winpthreads/src/thread.c':'0'*64}),
      lambda v:v['minimum_authority'].update(private_record_layout=v['minimum_authority']['private_record_layout'].replace('sizeof=480','sizeof=496')),
      lambda v:v['minimum_authority']['original_profile']['supplier'].update(_WIN32_WINNT=0xa00),
      lambda v:v['minimum_authority']['original']['files'].update({'lib/libwinpthread.a':'0'*64}),
      lambda v:v['minimum_authority']['files'].update({'lib/libpthread.a':'0'*64}),
      lambda v:v['minimum_authority']['public_headers'].pop('pthread.h'),
      lambda v:v['minimum_authority'].update(dll_exports=v['minimum_authority']['dll_exports'].replace('pthread_join','wrong_join')),
      lambda v:v['minimum_authority'].update(dll_imports=v['minimum_authority']['dll_imports'].replace('GetThreadId','WrongNative')),
      lambda v:v['minimum_authority']['library_symbols'].update({'lib/libwinpthread.a':'gesher_private_probe'}),
      lambda v:v['source_after'].update({'mingw-w64-libraries/winpthreads/src/sched.c':'0'*64}),
      lambda v:v['source_before'].pop('mingw-w64-libraries/winpthreads/src/rwlock.h'),
      lambda v:v['minimum_authority']['dll_symbol_objects'].pop('pthread_join'),
      lambda v:next(iter(v['minimum_authority']['dll_symbol_objects'].values())).update(actual_reading_path='/other/canonical/thread.o'),
      lambda v:next(iter(v['minimum_authority']['dll_symbol_objects'].values())).update(rva='UNRESOLVED'),
      lambda v:v['minimum_authority'].update(dll_compile_commands=[x.replace('-DDLL_EXPORT','') for x in v['minimum_authority']['dll_compile_commands']]),
      lambda v:v['minimum_authority'].update(dll_compile_commands=[x.replace('src/thread.c','src/sched.c') for x in v['minimum_authority']['dll_compile_commands']]),
      lambda v:v['minimum_authority'].update(dll_compile_commands=[x+' -D_WIN32_WINNT=0xa00' for x in v['minimum_authority']['dll_compile_commands']]),
      lambda v:v['commands'][0]['argv'].__setitem__(2,'/other/canonical/'+selected.NAME),
      lambda v:v['commands'][0].update(cwd='/other'),
      lambda v:v.update(commands=[]),
      lambda v:v['minimum_authority']['build_logs'].pop('supplier-profile.obj'),
      lambda v:v.update(notice_sha256='0'*64),
    ]
    for mutation in tests:flip(mutation)
    # A mismatching digest must fail independently of semantic field rejection.
    bad=copy.deepcopy(good);bad['minimum_authority_sha256']='0'*64
    try:selected.check_selected(bad,ctx)
    except AssertionError:pass
    else:raise AssertionError('Authority digest mismatch accepted')
    print('TCP_SELECTED_SUPPLIER_METADATA_PASS projection=SYNTHETIC_LOG_METADATA_ONLY semantic_mutants='+str(len(tests))+' authority_hash_negative=1 actual_selected_runtime_claim=false native_processes_started=0')

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('--diagnostic-receipt',type=Path,default=HERE.parents[1]/'tests/net-tcp-priority/minimum-receipt-411350.json');a=p.parse_args()
    raw=a.diagnostic_receipt.read_bytes();assert hashlib.sha256(raw).hexdigest()=='aba3e32beef66ec5ca6919e65f5e49f8b40f587599a8d2f43205e60bab873701'
    original=json.loads(raw);assert original['GITHUB_SHA']=='f0a8364afd3380eb0e25b8d82824dd1752f91e16' and original['GITHUB_RUN_ID']=='41135044960' and original['role']=='gesher-winpthreads-minimum-candidate-v1'
    check(original)
