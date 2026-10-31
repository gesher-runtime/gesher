"""Pure selected-supplier envelope validation; compilation is CI-only elsewhere."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import shlex
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/host-pthread-authority'))
import minimum_receipt as minimum

ROLE='gesher-winpthreads-selected-minimum-v1'
NAME='llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
URL='https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/'+NAME+'.tar.xz'
CONTEXT=('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY','GITHUB_REPOSITORY_ID')
AUTHORITY_CONTEXT=CONTEXT[:4]
LIBRARIES={'lib/libwinpthread.a','lib/libpthread.a','lib/libwinpthread.dll.a','lib/libpthread.dll.a','bin/libwinpthread-1.dll'}
HEADERS={'pthread.h','pthread_compat.h','pthread_signal.h','pthread_time.h','pthread_unistd.h','sched.h','semaphore.h'}
BUILD_LOGS={'configure-command.log','build-commands.log','install-commands.log','config.log','supplier-dll.map',
            'sdk-profile-macros.txt','supplier-profile-macros.txt','sdk-profile-machine.txt','supplier-profile-machine.txt',
            'sdk-profile.c','sdk-profile.obj','supplier-profile.obj'}


def authority_digest(authority):
    return hashlib.sha256(json.dumps(authority,sort_keys=True,indent=2).encode('utf-8')).hexdigest()


def digest(value):
    assert isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value),'Missing/invalid digest'


def normalized(path):
    assert isinstance(path,str) and path.startswith('/') and '\\' not in path
    assert posixpath.normpath(path)==path
    return path


def check_selected(supplier, context):
    """Validate portable metadata only; never resolve a Linux path on Windows."""
    assert set(context)==set(CONTEXT) and supplier['context']==context
    assert context['GITHUB_REPOSITORY']=='gesher-runtime/gesher' and context['GITHUB_REPOSITORY_ID']=='1426153186'
    assert re.fullmatch('[0-9a-f]{40}',context['GITHUB_SHA'])
    assert all(re.fullmatch('[1-9][0-9]*',context[k]) for k in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'))
    assert supplier['role']==ROLE and minimum.ROLE==ROLE and minimum.PIN['role']==ROLE
    assert supplier['supplier_selection']=='selected-minimum' and supplier['rebuilt_from_reference_source'] is True
    assert supplier['producer_job']=='compile' and supplier['patch_sha256'] is None
    assert supplier['ordered_patch_sha256']==minimum.PIN['ordered_patch_sha256']
    assert supplier['upstream']==minimum.PIN['upstream_commit']
    archive=supplier['compiler_archive']
    assert archive==dict(url=URL,sha256=minimum.PIN['compiler_archive_sha256'],bytes=84003044)
    compiler=normalized(supplier['compiler_root'])
    suffix='/tcp-selected-supplier/'+NAME
    assert compiler.endswith(suffix)
    workspace=compiler[:-len('/'+NAME)]
    source=workspace+'/source';build=source+'/build'

    authority=supplier['minimum_authority']
    assert authority_digest(authority)==supplier['minimum_authority_sha256']
    expected_context={k:context[k] for k in AUTHORITY_CONTEXT}
    assert all(authority[k]==v for k,v in expected_context.items())
    assert authority['role']==ROLE and authority['pin']==minimum.PIN
    assert authority['source_files']==minimum.PIN['output_source_sha256']
    assert authority['compiler_archive_sha256']==archive['sha256']
    minimum.validate_layout(authority['private_record_layout'])
    original=authority['original']
    assert original['context']==expected_context and original['role']=='publisher-reference-unmodified-v1'
    assert original['compiler_archive_sha256']==archive['sha256']
    assert set(authority['files'])==set(original['files'])==LIBRARIES
    assert supplier['libraries_before']==original['files'] and supplier['libraries_after']==authority['files']
    for inventory in (original['files'],authority['files']):
        for value in inventory.values():digest(value)
        assert inventory['lib/libpthread.a']==inventory['lib/libwinpthread.a']
        assert inventory['lib/libpthread.dll.a']==inventory['lib/libwinpthread.dll.a']
    assert original['files']['lib/libwinpthread.a']==minimum.PIN['publisher_static_sha256']
    assert authority['files']['lib/libwinpthread.a']!=original['files']['lib/libwinpthread.a']
    assert set(original['public_headers'])==set(authority['public_headers'])==HEADERS
    assert supplier['public_headers_before']==supplier['public_headers_after']==authority['public_headers']==original['public_headers']
    for value in authority['public_headers'].values():digest(value)
    assert authority['public_headers']['pthread.h']==minimum.PIN['source_sha256']['mingw-w64-libraries/winpthreads/include/pthread.h']
    assert minimum.exports(authority['dll_exports'])==original['dll_exports']==supplier['dll_exports']
    assert minimum.exports(original['dll_exports_raw'])==original['dll_exports']
    assert minimum.imports(authority['dll_imports'])==original['dll_import_names']==minimum.imports(original['dll_imports_raw'])
    assert re.search(r'\bGetThreadId\b',authority['dll_imports'])
    assert authority['original_profile']['sdk']==authority['original_profile']['supplier']
    profiles=authority['original_profile']['supplier']
    assert set(profiles)=={'_WIN32_WINNT','WINVER'} and profiles['_WIN32_WINNT']==0x0601 and profiles['WINVER']>=0x0600
    for key in ('compiler_driver_sha256','cxx_driver_sha256'):
        assert supplier[key]==authority[key]==original[key];digest(supplier[key])
    for key in ('compiler_real_sha256','underlying_clang_sha256','notice_sha256'):digest(supplier[key])
    assert supplier['compiler_version']==authority['compiler']
    assert hashlib.sha256(supplier['notice'].encode('utf-8')).hexdigest()==supplier['notice_sha256']

    before=supplier['source_before'];after=supplier['source_after']
    assert set(before)==set(after) and set(minimum.PIN['source_sha256'])<=set(before)
    for name,value in minimum.PIN['source_sha256'].items():assert before[name]==value
    for name,value in minimum.PIN['output_source_sha256'].items():assert after[name]==value
    assert {n for n in before if before[n]!=after[n]}=={'mingw-w64-libraries/winpthreads/src/thread.c'}
    assert before['mingw-w64-libraries/winpthreads/COPYING']==supplier['notice_sha256']
    for inventory in (before,after):
        for value in inventory.values():digest(value)
    assert set(authority['library_symbols'])=={'lib/libwinpthread.a','lib/libwinpthread.dll.a'}
    for raw in authority['library_symbols'].values():
        assert 'gesher_' not in raw.lower() and 'pthread_join' in raw and '_pthread_tryjoin' in raw
    assert {'pthread_create','pthread_join','pthread_self'}<=set(supplier['symbols'])
    assert len(supplier['symbols'])==len(set(supplier['symbols'])) and not any('gesher_' in n for n in supplier['symbols'])
    assert set(authority['build_logs'])==BUILD_LOGS
    for value in authority['build_logs'].values():digest(value)
    assert 'selected-minimum-factory.log' in supplier['logs']
    for value in supplier['logs'].values():digest(value)

    objects=authority['dll_symbol_objects']
    assert set(objects)==set(minimum.CORE)
    assert len({row['actual_reading_path'] for row in objects.values()})==1
    assert len({row['sha256'] for row in objects.values()})==1
    paths=set()
    for row in objects.values():
        path=normalized(row['actual_reading_path']);assert path.startswith(build+'/') and path.endswith('thread.o')
        assert re.fullmatch('[0-9a-fA-F]+',row['rva']) and row['input'];digest(row['sha256']);paths.add(path)
        observed=row['input'] if row['input'].startswith('/') else posixpath.join(build,row['input'])
        assert posixpath.normpath(observed)==path
    compiled=set()
    assert authority['dll_compile_commands']
    for command in authority['dll_compile_commands']:
        argv=shlex.split(command)
        assert compiler+'/bin/x86_64-w64-mingw32-clang' in argv
        assert all(flag in argv for flag in ('-DHAVE_CONFIG_H','-DIN_WINPTHREAD','-DDLL_EXPORT','-DPIC','-g','-O2','-mguard=cf','-Werror','-Xclang','-fdump-record-layouts','-c','-o'))
        assert not any(re.match(r'-D(?:GESHER_|WINPTHREAD_ROUTING|_WIN32_WINNT|WINVER)',v) for v in argv)
        out=argv[argv.index('-o')+1];out=out if out.startswith('/') else posixpath.join(build,out)
        compiled.add(posixpath.normpath(out))
        actual_source=argv[argv.index('-c')+1]
        actual_source=actual_source if actual_source.startswith('/') else posixpath.join(build,actual_source)
        assert posixpath.normpath(actual_source)==source+'/mingw-w64-libraries/winpthreads/src/thread.c'
    assert paths<=compiled
    expected_factory=['bash',workspace.rsplit('/',1)[0]+'/tools/host-pthread-authority/build-minimum-supplier.sh',compiler,source]
    factory=[row for row in supplier['commands'] if row['log']=='selected-minimum-factory.log']
    assert len(factory)==1 and factory[0]['argv']==expected_factory and factory[0]['cwd']==workspace.rsplit('/',1)[0]


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--manifest',type=Path,required=True);args=parser.parse_args()
    context={k:os.environ[k] for k in CONTEXT}
    assert os.environ.get('GITHUB_ACTIONS')=='true'
    if os.environ.get('GITHUB_REF')=='refs/heads/wip/coherent-selected-tcp':
        sys.path.insert(0,str(ROOT/'tools/toolchain'))
        import coherent_tcp_context as coherent
        coherent.context(os.environ,'windows')
    else:
        assert os.environ['GITHUB_REF']=='refs/heads/wip/tcp-minimum-selection'
        assert os.environ['GITHUB_WORKFLOW']=='net-tcp-selected-component'
    manifest=json.loads(args.manifest.read_text())
    assert manifest['repository']==context['GITHUB_REPOSITORY'] and str(manifest['repository_id'])==context['GITHUB_REPOSITORY_ID']
    assert manifest['source']==context['GITHUB_SHA'] and str(manifest['run'])==context['GITHUB_RUN_ID'] and str(manifest['attempt'])==context['GITHUB_RUN_ATTEMPT']
    assert manifest['supplier_selection']=='selected-minimum' and manifest['producer_job']=='compile'
    check_selected(manifest['supplier'],context)
    print('Verified selected original-profile supplier: twelve exact sources/two patches,480allfields,601typedSDK,unchanged publicABI,aliases,actual DLL source object; product adoption not inferred')


if __name__=='__main__':main()
