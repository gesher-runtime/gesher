"""CI-only genuine compiler TLS consumers; keep supplier variants distinct."""
import hashlib,json,os,posixpath,re,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
ARCHIVE='5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
SOURCES=['pthread_late_cpp_main.cc','pthread_late_cpp_a.cc','pthread_late_cpp_b.cc','pthread_late_cpp_shared.h']
ROLES={'published':('baseline_compile','publisher-archive-unmodified'),
       'production':('baseline_compile','gesher-winpthreads-authority-v1'),
       'deferred':('probe_deferred_compile','gesher-winpthreads-authority-routing-probe-deferred-v1')}
FLAGS=['-std=gnu++20','-O2','-g','-Wall','-Wextra','-Werror','-mno-ms-bitfields','-fno-exceptions','-fsized-deallocation','-D_WIN32_WINNT=0x0A00','-DWINVER=0x0A00','-D_FILE_OFFSET_BITS=64','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-DNOGDI','-D_USE_MATH_DEFINES','-D__STDC_FORMAT_MACROS','-D__STDC_CONSTANT_MACROS','-D__STDC_LIMIT_MACROS','-D_POSIX_THREAD_SAFE_FUNCTIONS']
PTHREAD_SYMBOLS=('pthread_self','pthread_create','pthread_join','pthread_exit','pthread_key_create','pthread_key_delete','pthread_getspecific','pthread_setspecific','_pthread_cleanup_dest','__dyn_tls_pthread','__xl_f')

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def member_bindings(map_text,link_text,inputs):
    normal=lambda p:posixpath.normpath(p.replace('\\','/'))
    allowed={normal(v['path']):v['sha256'] for v in inputs.values()}
    loaded=[]
    for archive,member,reason in re.findall(r'Loaded ([^\r\n]+?\.a)\(([^()\r\n]+)\) for ([^\r\n]+)',link_text):
        archive=normal(archive)
        if Path(archive).name in inputs:
            assert archive in allowed,'Pthread member loaded outside the exact variant: '+archive
        loaded.append((archive,member,reason))
    assert any(x[0] in allowed for x in loaded),'No actual pthread archive-member extraction witness'
    owners={};owner=None
    for line in map_text.splitlines():
        match=re.fullmatch(r'([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9]+ (.*)',line)
        if not match:continue
        tail=match.group(2)
        if ':(' in tail:
            owner=tail.strip().rsplit(':(',1)[0]
        elif tail.startswith('                '):
            symbol=tail.strip()
            if symbol in PTHREAD_SYMBOLS:
                definition=(owner,match.group(1).lower())
                # LLD iterates every input symbol table, including references
                # resolved to the same DefinedRegular. Identical repeats carry
                # the same provenance; conflicting address or owner cannot.
                assert symbol not in owners or owners[symbol]==definition,'Conflicting actual symbol ownership/address: '+symbol
                owners[symbol]=definition
        else:owner=None
    assert set(owners)==set(PTHREAD_SYMBOLS),'Missing actual pthread/TLS symbol ownership: '+str(set(PTHREAD_SYMBOLS)-set(owners))
    result={}
    for symbol,(owner,address) in owners.items():
        assert owner,'No input section owns symbol: '+symbol
        matches=[x for x in loaded if Path(x[1]).name==Path(owner).name]
        assert len(matches)==1,'Ambiguous or non-variant symbol member: '+symbol+' '+str(owner)
        archive,member,reason=matches[0]
        assert archive in allowed,'Symbol supplied by another variant/archive: '+symbol+' '+archive
        result[symbol]=dict(archive=archive,archive_sha256=allowed[archive],member=member,extracted_for=reason,map_input=owner,map_rva=address)
    return result
def context():
    assert os.environ['GITHUB_ACTIONS']=='true' and os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher'
    assert os.environ['GITHUB_REF']=='refs/heads/wip/pthread-process-routing'
    assert os.environ['GITHUB_WORKFLOW']=='pthread process identity routing probe'
    assert subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()==os.environ['GITHUB_SHA']
    subprocess.check_call(['git','diff','--exit-code','HEAD','--'])
    return {k:os.environ[k] for k in ('GITHUB_SHA','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT','GITHUB_REPOSITORY')}
def build(llvm,role):
    ctx=context();producer,variant=ROLES[role];assert os.environ['GITHUB_JOB']==producer
    dist=ROOT/('late-cpp-'+role);dist.mkdir()
    archive=llvm.parent/'compiler.tar.xz';assert sha(archive)==ARCHIVE
    driver=llvm/'bin/x86_64-w64-mingw32-clang++';lib=llvm/'x86_64-w64-mingw32/lib/libwinpthread.a'
    dll=llvm/'x86_64-w64-mingw32/bin/libwinpthread-1.dll';provider=llvm/'gesher-winpthreads-authority.json'
    pthread_inputs={name:dict(path=str(llvm/'x86_64-w64-mingw32/lib'/name),sha256=sha(llvm/'x86_64-w64-mingw32/lib'/name)) for name in ('libwinpthread.a','libpthread.a')}
    for name,record in pthread_inputs.items():print('CPP_TLS_PTHREAD_INPUT role='+role+' archive='+name+' path='+record['path']+' sha256='+record['sha256'],flush=True)
    assert pthread_inputs['libwinpthread.a']['sha256']==pthread_inputs['libpthread.a']['sha256'],'Static pthread alias is not the selected variant archive'
    if role=='published':
        assert not provider.exists()
        (dist/'provider.json').write_text(json.dumps(dict(variant=variant,archive=ARCHIVE,cxx=sha(driver),library=sha(lib),dll=sha(dll)),indent=2))
    else:
        data=json.loads(provider.read_text());assert data['role']==variant and all(data[k]==v for k,v in ctx.items())
        assert data['compiler_archive_sha256']==ARCHIVE and data['cxx_driver_sha256']==sha(driver)
        assert data['files']['lib/libwinpthread.a']==sha(lib) and data['files']['bin/libwinpthread-1.dll']==sha(dll)
        assert data['files']['lib/libpthread.a']==pthread_inputs['libpthread.a']['sha256']
        shutil.copyfile(provider,dist/'provider.json')
    commands=[];objects=[]
    for name in SOURCES[:3]:
        obj=dist/(Path(name).stem+'.obj');argv=[str(driver)]+FLAGS+['-c',str(ROOT/'tests/native'/name),'-o',str(obj)]
        subprocess.check_call(argv);commands.append(argv);objects.append(obj)
    exe=dist/'late-cpp.exe';argv=[str(driver),'-static','-pthread']+[str(x) for x in objects]+[str(lib),'-Wl,--verbose','-Wl,-Map,'+str(dist/'late-cpp.map'),'-o',str(exe)]
    link=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    print(link.stdout,end='',flush=True);(dist/'link.txt').write_text(link.stdout);link.check_returncode();commands.append(argv)
    map_text=(dist/'late-cpp.map').read_text()
    try:bindings=member_bindings(map_text,link.stdout,pthread_inputs)
    except AssertionError as error:
        print('CPP_TLS_MEMBER_UNRESOLVED role='+role+' '+str(error),flush=True)
        print('CPP_TLS_RAW_MAP_BEGIN role='+role+'\n'+map_text+'CPP_TLS_RAW_MAP_END',flush=True)
        raise
    for symbol,record in bindings.items():print('CPP_TLS_PTHREAD_MEMBER role='+role+' symbol='+symbol+' '+json.dumps(record,sort_keys=True),flush=True)
    imports=subprocess.check_output([str(llvm/'bin/llvm-readobj'),'--coff-imports',str(exe)],text=True)
    assert 'libwinpthread-1.dll' not in imports.lower() and 'libc++' not in imports.lower()
    (dist/'imports.txt').write_text(imports)
    symbols=subprocess.check_output([str(llvm/'bin/llvm-nm'),'-a','-C']+[str(x) for x in objects]+[str(exe)],text=True)
    (dist/'tls-symbols.txt').write_text(symbols)
    matches=[x for x in symbols.splitlines() if re.search(r'emutls|thread_atexit|tlregdtor|dyn_tls|Probe[AB]|LateCppRecord',x)]
    print('CPP_TLS_MODEL role='+role+' matches='+str(len(matches)),flush=True)
    for line in matches[:200]:print('CPP_TLS_MODEL_SYMBOL '+line,flush=True)
    data=dict(context=ctx,role=role,variant=variant,producer=producer,archive=ARCHIVE,compiler_root=str(llvm),workspace=str(ROOT),driver=str(driver),driver_sha256=sha(driver),library_sha256=sha(lib),dll_sha256=sha(dll),pthread_archive_inputs=pthread_inputs,version=subprocess.check_output([str(driver),'--version'],text=True),commands=commands,sources={n:sha(ROOT/'tests/native'/n) for n in SOURCES},archives={p.name:sha(p) for p in (llvm/'x86_64-w64-mingw32/lib').glob('*.a')},members={p.name:sha(p) for p in dist.iterdir()})
    data['pthread_symbol_members']=bindings
    (dist/'bundle.json').write_text(json.dumps(data,sort_keys=True,indent=2))
    print('CPP_TLS_BUILD role='+role+' variant='+variant+' archive='+ARCHIVE+' cxx='+sha(driver)+' pthread='+sha(lib),flush=True)
def verify(role):
    ctx=context();producer,variant=ROLES[role];dist=ROOT/('late-cpp-'+role);data=json.loads((dist/'bundle.json').read_text())
    assert data['context']==ctx and data['role']==role and data['variant']==variant and data['producer']==producer and data['archive']==ARCHIVE
    expected={'provider.json','late-cpp.exe','imports.txt','tls-symbols.txt','link.txt','late-cpp.map'}|{Path(n).stem+'.obj' for n in SOURCES[:3]}
    assert set(data['members'])==expected and {p.name for p in dist.iterdir()}==expected|{'bundle.json'}
    assert all(sha(dist/n)==v for n,v in data['members'].items())
    assert data['sources']=={n:sha(ROOT/'tests/native'/n) for n in SOURCES}
    assert data['driver']==data['compiler_root']+'/bin/x86_64-w64-mingw32-clang++'
    assert data['driver_sha256'] and data['archives']['libwinpthread.a']==data['library_sha256']
    assert data['pthread_archive_inputs']=={name:dict(path=data['compiler_root']+'/x86_64-w64-mingw32/lib/'+name,sha256=data['archives'][name]) for name in ('libwinpthread.a','libpthread.a')}
    assert data['archives']['libpthread.a']==data['library_sha256']
    assert data['pthread_symbol_members']==member_bindings((dist/'late-cpp.map').read_text(),(dist/'link.txt').read_text(),data['pthread_archive_inputs'])
    assert all(re.fullmatch('[0-9a-f]{64}',v) for v in data['archives'].values())
    expected_commands=[]
    for name in SOURCES[:3]:expected_commands.append([data['driver']]+FLAGS+['-c',data['workspace']+'/tests/native/'+name,'-o',data['workspace']+'/late-cpp-'+role+'/'+Path(name).stem+'.obj'])
    expected_commands.append([data['driver'],'-static','-pthread']+[data['workspace']+'/late-cpp-'+role+'/'+Path(n).stem+'.obj' for n in SOURCES[:3]]+[data['compiler_root']+'/x86_64-w64-mingw32/lib/libwinpthread.a','-Wl,--verbose','-Wl,-Map,'+data['workspace']+'/late-cpp-'+role+'/late-cpp.map','-o',data['workspace']+'/late-cpp-'+role+'/late-cpp.exe'])
    assert data['commands']==expected_commands
    provider=json.loads((dist/'provider.json').read_text())
    if role=='published':assert provider==dict(variant=variant,archive=ARCHIVE,cxx=data['driver_sha256'],library=data['library_sha256'],dll=data['dll_sha256'])
    else:
        assert provider['role']==variant and all(provider[k]==v for k,v in ctx.items())
        assert provider['compiler_archive_sha256']==ARCHIVE and provider['cxx_driver_sha256']==data['driver_sha256']
        assert provider['files']['lib/libwinpthread.a']==data['library_sha256'] and provider['files']['bin/libwinpthread-1.dll']==data['dll_sha256']
        assert provider['files']['lib/libpthread.a']==data['pthread_archive_inputs']['libpthread.a']['sha256']
    imports=(dist/'imports.txt').read_text().lower();assert 'libwinpthread-1.dll' not in imports and 'libc++' not in imports
    print('Verified distinct C++ TLS variant '+role+' producer='+producer+' same source/run/attempt and complete members',flush=True)
if __name__=='__main__':
    if sys.argv[1]=='build':build(Path(sys.argv[2]),sys.argv[3])
    elif sys.argv[1]=='verify':verify(sys.argv[2])
    else:raise SystemExit(2)
