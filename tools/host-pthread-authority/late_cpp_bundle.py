"""CI-only genuine compiler TLS consumers; keep supplier variants distinct."""
import hashlib,json,os,posixpath,re,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
ARCHIVE='5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
SOURCES=['pthread_late_cpp_main.cc','pthread_late_cpp_a.cc','pthread_late_cpp_b.cc','pthread_late_cpp_shared.h']
ROLES={'published':('baseline_compile','publisher-archive-unmodified'),
       'production':('baseline_compile','gesher-winpthreads-authority-v1'),
       'deferred':('probe_deferred_compile','gesher-winpthreads-authority-routing-probe-deferred-v1'),
       'minimum-candidate':('minimum_compile','gesher-winpthreads-minimum-candidate-v1')}
FLAGS=['-std=gnu++20','-O2','-g','-Wall','-Wextra','-Werror','-mno-ms-bitfields','-fno-exceptions','-fsized-deallocation','-D_WIN32_WINNT=0x0A00','-DWINVER=0x0A00','-D_FILE_OFFSET_BITS=64','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-DNOGDI','-D_USE_MATH_DEFINES','-D__STDC_FORMAT_MACROS','-D__STDC_CONSTANT_MACROS','-D__STDC_LIMIT_MACROS','-D_POSIX_THREAD_SAFE_FUNCTIONS']
TLS_OWNER_SYMBOLS=('gesher_tls_reaper_start_v1','gesher_tls_retirement_watch_v1','gesher_tls_retirement_counts_v1','gesher_tls_retirement_snapshot_v1')
PTHREAD_SYMBOLS=('pthread_self','pthread_create','pthread_join','pthread_exit','pthread_key_create','pthread_key_delete','pthread_getspecific','pthread_setspecific','_pthread_cleanup_dest','__dyn_tls_pthread','__xl_f')

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def member_bindings(map_text,link_text,inputs):
    normal=lambda p:posixpath.normpath(p.replace('\\','/'))
    allowed={normal(v['path']):v['sha256'] for v in inputs.values()}
    origins={}
    for path in re.findall(r'\bReading ([^\r\n]+\.a)\r?$',link_text,re.MULTILINE):
        path=normal(path)
        if posixpath.isabs(path):origins.setdefault(Path(path).name,set()).add(path)
    loaded=[]
    for archive,member,reason in re.findall(r'Loaded ([^\r\n]+?\.a)\(([^()\r\n]+)\) for ([^\r\n]+)',link_text):
        archive=normal(archive)
        # This LLD prints full paths for archive Reading, but only basenames
        # for member Loaded. Resolve that abbreviation only from one actual
        # full-path Reading witness, never by prefixing an expected directory.
        if archive in inputs:
            candidates=origins.get(archive,set())
            assert len(candidates)==1,'Missing/ambiguous actual archive Reading origin: '+archive
            archive=next(iter(candidates))
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
def check_owner_symbols(role,symbols):
    found={name for name in TLS_OWNER_SYMBOLS if re.search(r'\b'+name+r'$',symbols,re.MULTILINE)}
    assert found==(set(TLS_OWNER_SYMBOLS) if role=='deferred' else set()),'Kernel TLS owner consumer symbol isolation mismatch: '+str(found)

def check_trace(role,text):
    assert role in ROLES
    def one(pattern):
        matches=re.findall('(?m)^'+pattern+r'\r?$',text)
        assert len(matches)==1,'Missing/ambiguous actual C++ trace: '+pattern
        return matches[0]
    expected_cases=6 if role=='deferred' else 4
    one('CPP_TLS_RESULT cases='+str(expected_cases)+' failures=0 conformant=1')
    modes=('pthread_return','pthread_exit','std_thread_return','native_ExitThread')
    for mode in modes:
        one('CPP_TLS_CASE mode='+mode+' constructed=2 a=1 b=1 mismatch=0 a_done=1 b_done=1 conformant=1')
        one('CPP_TLS_KERNEL mode='+mode+' signaled=1 exit=[0-9]+ owned_handle_closed=1')
        # The callback count is explicitly observational. Only successful live
        # access is required; this gate never relabels late TSD ordering green.
        one('CPP_TLS_LATE_KEY mode='+mode+' successful_set_get=2 destructor_calls=[0-9]+ observation_only=1')
    for mode in modes[:3]:
        one('CPP_TLS_JOIN mode='+mode+' phase=join-return a=1 b=1 mismatch=0 sequence=2 a_done=1 b_done=1 conformant=1')
        one('CPP_TLS_JOIN_KERNEL mode='+mode+' phase=join-return wait=(0|258) observation_only=1')
    present=1 if role=='deferred' else 0
    persistent=2*present
    original,baseline,actual_persistent=map(int,one('CPP_TLS_OWNER_BOOTSTRAP present='+str(present)+' original=([0-9]+) baseline=([0-9]+) persistent_handles=([0-9]+)'))
    assert baseline==original+persistent and actual_persistent==persistent,'Bootstrap persistent handle accounting mismatch'
    origin2,base2,after,persist2=map(int,one('CPP_TLS_OWNER_HANDLES original=([0-9]+) baseline=([0-9]+) after=([0-9]+) persistent_handles=([0-9]+) original_total_bound=8 bounded=1'))
    assert (origin2,base2,persist2)==(original,baseline,persistent) and after<=baseline+8 and after<=original+8,'Original total handle bound or identities changed'
    before2,after2=map(int,one('CPP_TLS_HANDLES before=([0-9]+) after=([0-9]+) original_bound=8 bounded=1'))
    assert (before2,after2)==(baseline,after),'Existing handle gate disagrees'
    held=('owned_held_join','owned_held_detach')
    if role=='deferred':
        body_native,body_id=one('CPP_TLS_BODY mode=owned_held_join native=([0-9]+) pthread=([0-9]+) constructed=2')
        joiner_native=one('CPP_TLS_JOINER_READY mode=owned_held_join native=([0-9]+) before_target=1 body_entered=1 owned_native_handle=1')
        released_native,target_id=one('CPP_TLS_JOINER_RELEASE mode=owned_held_join native=([0-9]+) target=([0-9]+) after_callback_held=1')
        assert int(joiner_native)>0 and joiner_native==released_native and int(target_id)>0 and body_id==target_id and int(body_native)>0 and body_native!=joiner_native,'Prestarted native joiner or target publication mismatch'
        def position(label):
            assert label in text,'Missing prestarted joiner order witness: '+label
            return text.index(label)
        assert position('CPP_TLS_JOINER_READY')<position('CPP_TLS_BODY mode=owned_held_join')<position('CPP_TLS_HELD_ENTER mode=owned_held_join')<position('CPP_TLS_JOINER_RELEASE')<position('CPP_TLS_HELD_BEFORE mode=owned_held_join'),'Joiner was not actually started before target/callback hold'
        for mode in held:
            one('CPP_TLS_HELD_CONTROL mode='+mode+' phase=owned-EXITING explicit_enable=1 self_cancel_esrch=1 pending=0 testcancel_returned=1 restored_disabled=1 diagnostic_only=1')
            tu,early=one('CPP_TLS_HELD_ENTER mode='+mode+' tu=([AB]) early_budget=256 actual_early=([0-9]+)')
            assert int(early)==256
            scans=one('CPP_TLS_HELD_BEFORE mode='+mode+' original_identity=1 tls_owner=1 lifecycle=3 ended=0 kernel=258 clock_live=1 event_live=1 spin_live=1 scans=([0-9]+) finalized=0 released=0 pool=0 early_budget=256 external_delete=1')
            assert int(scans)>0,'No actual unsignaled worker scan while genuine callback held'
            one('CPP_TLS_HELD_AFTER mode='+mode+' kernel_signaled=1 finalized=1 released=1 clock_destroy=1 event_close=1 spin_destroy=1 pool_retire=1 original_identity=1 a_done=1 b_done=1 late_key_explicit_clear=1')
    else:
        assert 'CPP_TLS_HELD_' not in text,'Diagnostic held case leaked into another supplier role'
    assert 'FAIL ' not in text,'C++ fixture failure trace'

def selftest_trace():
    def fixture(role):
        deferred=role=='deferred'; original=60; baseline=original+(2 if deferred else 0); after=64
        lines=['CPP_TLS_RESULT cases='+str(6 if deferred else 4)+' failures=0 conformant=1']
        for mode in ('pthread_return','pthread_exit','std_thread_return','native_ExitThread'):
            lines += ['CPP_TLS_CASE mode='+mode+' constructed=2 a=1 b=1 mismatch=0 a_done=1 b_done=1 conformant=1',
                'CPP_TLS_KERNEL mode='+mode+' signaled=1 exit=61 owned_handle_closed=1',
                'CPP_TLS_LATE_KEY mode='+mode+' successful_set_get=2 destructor_calls=42 observation_only=1']
        for mode in ('pthread_return','pthread_exit','std_thread_return'):
            lines += ['CPP_TLS_JOIN mode='+mode+' phase=join-return a=1 b=1 mismatch=0 sequence=2 a_done=1 b_done=1 conformant=1',
                'CPP_TLS_JOIN_KERNEL mode='+mode+' phase=join-return wait=258 observation_only=1']
        lines += [f'CPP_TLS_OWNER_BOOTSTRAP present={int(deferred)} original={original} baseline={baseline} persistent_handles={baseline-original}',
            f'CPP_TLS_OWNER_HANDLES original={original} baseline={baseline} after={after} persistent_handles={baseline-original} original_total_bound=8 bounded=1',
            f'CPP_TLS_HANDLES before={baseline} after={after} original_bound=8 bounded=1']
        if deferred:
            lines += ['CPP_TLS_JOINER_READY mode=owned_held_join native=111 before_target=1 body_entered=1 owned_native_handle=1',
                'CPP_TLS_BODY mode=owned_held_join native=222 pthread=6 constructed=2']
            for mode in ('owned_held_join','owned_held_detach'):
                lines += ['CPP_TLS_HELD_CONTROL mode='+mode+' phase=owned-EXITING explicit_enable=1 self_cancel_esrch=1 pending=0 testcancel_returned=1 restored_disabled=1 diagnostic_only=1',
                    'CPP_TLS_HELD_ENTER mode='+mode+' tu=B early_budget=256 actual_early=256',
                    'CPP_TLS_HELD_BEFORE mode='+mode+' original_identity=1 tls_owner=1 lifecycle=3 ended=0 kernel=258 clock_live=1 event_live=1 spin_live=1 scans=2 finalized=0 released=0 pool=0 early_budget=256 external_delete=1',
                    'CPP_TLS_HELD_AFTER mode='+mode+' kernel_signaled=1 finalized=1 released=1 clock_destroy=1 event_close=1 spin_destroy=1 pool_retire=1 original_identity=1 a_done=1 b_done=1 late_key_explicit_clear=1']
            enter=lines.index('CPP_TLS_HELD_ENTER mode=owned_held_join tu=B early_budget=256 actual_early=256')
            lines.insert(enter+1,'CPP_TLS_JOINER_RELEASE mode=owned_held_join native=111 target=6 after_callback_held=1')
        return lines
    rejected=0
    def reject(role,text):
        nonlocal rejected
        try:check_trace(role,text)
        except AssertionError:rejected+=1
        else:raise AssertionError('Source-only metadata negative was accepted')
    for role in ROLES:
        lines=fixture(role); text='\n'.join(lines)+'\n'; check_trace(role,text)
        # Every actual required row is mandatory, exactly once.
        for i in range(len(lines)):
            reject(role,'\n'.join(lines[:i]+lines[i+1:])+'\n')
            reject(role,text+lines[i]+'\n')
        reject(role,text.replace('successful_set_get=2','successful_set_get=1',1))
        reject(role,text.replace('a_done=1','a_done=0',1))
        reject(role,text.replace('after=64','after=69'))
        reject(role,text.replace('original_total_bound=8','original_total_bound=9'))
        if role=='deferred':
            for old,new in [('native=111','native=0'),('target=6','target=0'),('pending=0','pending=1'),('self_cancel_esrch=1','self_cancel_esrch=0'),('scans=2','scans=0'),('actual_early=256','actual_early=257'),('kernel=258','kernel=0'),('clock_destroy=1','clock_destroy=2'),('spin_destroy=1','spin_destroy=0'),('pool_retire=1','pool_retire=0'),('late_key_explicit_clear=1','late_key_explicit_clear=0')]:reject(role,text.replace(old,new,1))
            for first,second in [('CPP_TLS_JOINER_READY','CPP_TLS_BODY mode=owned_held_join'),('CPP_TLS_JOINER_RELEASE','CPP_TLS_HELD_BEFORE mode=owned_held_join')]:
                reordered=list(lines)
                a=next(i for i,line in enumerate(reordered) if line.startswith(first))
                b=next(i for i,line in enumerate(reordered) if line.startswith(second))
                reordered[a],reordered[b]=reordered[b],reordered[a]
                reject(role,'\n'.join(reordered)+'\n')
        else:reject(role,text+'CPP_TLS_HELD_AFTER unexpected=1\n')
        symbols='\n'.join('0000 T '+n for n in TLS_OWNER_SYMBOLS) if role=='deferred' else ''
        check_owner_symbols(role,symbols)
        for name in TLS_OWNER_SYMBOLS:
            bad=symbols.replace('0000 T '+name,'') if role=='deferred' else '0000 T '+name
            try:check_owner_symbols(role,bad)
            except AssertionError:rejected+=1
            else:raise AssertionError('Wrong supplier export isolation accepted')
    print('PASS C++ source-only metadata: roles='+str(len(ROLES))+', observational late TSD count remains unrestricted, ablation/conflict/role negatives='+str(rejected)+'; no native execution')

def check_phase_observations(role,text):
    assert role in ROLES
    def one(pattern):
        matches=re.findall('(?m)^'+pattern+r'\r?$',text)
        assert len(matches)==1,'Missing/ambiguous phase observation identity: '+pattern
        return matches[0]
    matches=re.findall(r'(?m)^CPP_TLS_HANDLE_PHASE mode=([A-Za-z_]+) phase=([A-Za-z_]+) count=([0-9]+) observation_only=1\r?$',text)
    assert len(matches)==len(re.findall(r'(?m)^CPP_TLS_HANDLE_PHASE[^\r\n]*\r?$',text)),'Malformed actual phase row'
    phases={}
    for mode,phase,count in matches:
        assert (mode,phase) not in phases,'Repeated actual handle observation phase'
        phases[mode,phase]=int(count)
    expected={('main',phase) for phase in ('initial','after_owner','after_late_key_create','before_late_key_delete','after_late_key_delete','final_after_drain')}
    modes=('pthread_return','pthread_exit','std_thread_return','native_ExitThread')
    for mode in modes:
        expected|={(mode,phase) for phase in ('before_start','body_after_native_duplicate','late_setget_A','late_setget_B','after_owned_kernel_handle_close')}
        expected.add((mode,'after_native_thread_wait' if mode=='native_ExitThread' else 'after_join_snapshot'))
    if role=='deferred':
        expected|={(mode,phase) for mode in ('owned_held_join','owned_held_detach') for phase in ('body_after_native_duplicate','late_setget_A','late_setget_B')}
    assert set(phases)==expected,'Missing/unexpected actual phase observations: '+str(expected-set(phases))+' / '+str(set(phases)-expected)
    present,original,baseline,persistent=map(int,one(r'CPP_TLS_OWNER_BOOTSTRAP present=([01]) original=([0-9]+) baseline=([0-9]+) persistent_handles=([0-9]+)'))
    assert present==int(role=='deferred') and persistent==2*present and baseline==original+persistent
    origin2,base2,after,persist2,bounded=map(int,one(r'CPP_TLS_OWNER_HANDLES original=([0-9]+) baseline=([0-9]+) after=([0-9]+) persistent_handles=([0-9]+) original_total_bound=8 bounded=([01])'))
    assert (origin2,base2,persist2)==(original,baseline,persistent)
    assert bounded==int(after<=original+8 and after<=baseline+8),'Original strict bound output was changed'
    assert phases['main','initial']==original and phases['main','after_owner']==baseline and phases['main','final_after_drain']==after
    # Counts may rise or fall and the strict bound may be RED. No cleanup cause,
    # monotonicity, callback ordering or conformance is inferred from this data.
    return dict(role=role,phases=len(phases),original_bound=8,actual_bounded=bounded,conformance='NOT_ASSERTED')

def selftest_phase_observations():
    negative=0
    for role in ROLES:
        deferred=role=='deferred';original=60;baseline=60+(2 if deferred else 0)
        after=70 if role=='published' else 66;bounded=int(after<=68)
        lines=[f'CPP_TLS_OWNER_BOOTSTRAP present={int(deferred)} original={original} baseline={baseline} persistent_handles={baseline-original}',
            f'CPP_TLS_OWNER_HANDLES original={original} baseline={baseline} after={after} persistent_handles={baseline-original} original_total_bound=8 bounded={bounded}']
        required=[('main',phase) for phase in ('initial','after_owner','after_late_key_create','before_late_key_delete','after_late_key_delete','final_after_drain')]
        for mode in ('pthread_return','pthread_exit','std_thread_return','native_ExitThread'):
            required += [(mode,phase) for phase in ('before_start','body_after_native_duplicate','late_setget_A','late_setget_B','after_owned_kernel_handle_close')]
            required += [(mode,'after_native_thread_wait' if mode=='native_ExitThread' else 'after_join_snapshot')]
        if deferred:required += [(mode,phase) for mode in ('owned_held_join','owned_held_detach') for phase in ('body_after_native_duplicate','late_setget_A','late_setget_B')]
        for mode,phase in required:
            count=original if (mode,phase)==('main','initial') else baseline if (mode,phase)==('main','after_owner') else after if (mode,phase)==('main','final_after_drain') else 81
            lines += [f'CPP_TLS_HANDLE_PHASE mode={mode} phase={phase} count={count} observation_only=1']
        text='\n'.join(lines)+'\n';result=check_phase_observations(role,text)
        assert result['actual_bounded']==bounded and result['conformance']=='NOT_ASSERTED'
        controls=['\n'.join(lines[:i]+lines[i+1:])+'\n' for i in range(len(lines))]
        controls += [text+line+'\n' for line in lines]
        controls += [text.replace('original_total_bound=8','original_total_bound=10'),text.replace('phase=initial count=60','phase=initial count=61')]
        controls += [text+'CPP_TLS_HANDLE_PHASE mode=main phase=unknown count=1 observation_only=1\n',text+'CPP_TLS_HANDLE_PHASE mode=main phase=initial count=bad observation_only=1\n']
        for bad in controls:
            try:check_phase_observations(role,bad)
            except AssertionError:negative+=1
            else:raise AssertionError('Incomplete/changed initial bound phase metadata accepted')
    print('PASS source-only phase metadata missing/duplicate/initial/bound negatives='+str(negative)+'; actual boundRED staysRED, no conformance inferred')

def package_build_metadata(llvm,role,ctx,library):
    versions=llvm/'versions.txt'
    record={'present':False}
    if versions.exists() or versions.is_symlink():
        assert versions.is_file() and versions.resolve().is_relative_to(llvm.resolve()),'Package versions file escapes authenticated extracted compiler'
        raw=versions.read_bytes();assert len(raw)<=1024*1024,'Unexpectedly large package versions metadata'
        text=raw.decode('utf-8')
        record=dict(present=True,sha256=hashlib.sha256(raw).hexdigest(),size=len(raw),text=text,
            observed_assignments=re.findall(r'(?m)^([A-Z_]+)=([^\r\n]*)\r?$',text))
    return dict(context=ctx,role=role,variant=ROLES[role][1],compiler_archive_sha256=ARCHIVE,
        compiler_root=str(llvm),selected_pthread_sha256=library,
        source_attestation='UNRESOLVED-package-observations-only',
        root_entries=sorted(p.name for p in llvm.iterdir()),versions=record)

def check_package_metadata(data,role,ctx,compiler_root,library):
    assert set(data)=={'context','role','variant','compiler_archive_sha256','compiler_root','selected_pthread_sha256','source_attestation','root_entries','versions'}
    assert data['context']==ctx and data['role']==role and data['variant']==ROLES[role][1]
    assert data['compiler_archive_sha256']==ARCHIVE and data['compiler_root']==compiler_root and data['selected_pthread_sha256']==library
    assert data['source_attestation']=='UNRESOLVED-package-observations-only'
    entries=data['root_entries'];assert isinstance(entries,list) and entries==sorted(set(entries)) and all(isinstance(n,str) and n not in ('.','..') and '/' not in n and '\\' not in n for n in entries)
    record=data['versions'];assert isinstance(record,dict) and isinstance(record.get('present'),bool)
    if not record['present']:assert record=={'present':False} and 'versions.txt' not in entries
    else:
        assert set(record)=={'present','sha256','size','text','observed_assignments'} and 'versions.txt' in entries
        raw=record['text'].encode('utf-8');assert len(raw)==record['size'] and len(raw)<=1024*1024 and hashlib.sha256(raw).hexdigest()==record['sha256']
        assert [list(x) for x in re.findall(r'(?m)^([A-Z_]+)=([^\r\n]*)\r?$',record['text'])]==[list(x) for x in record['observed_assignments']]

def selftest_package_metadata():
    import tempfile,copy
    ctx=dict(GITHUB_SHA='1'*40,GITHUB_RUN_ID='synthetic-only',GITHUB_RUN_ATTEMPT='1',GITHUB_REPOSITORY='gesher-runtime/gesher')
    negative=0
    with tempfile.TemporaryDirectory(prefix='gesher-package-metadata-') as folder:
        llvm=Path(folder);library='2'*64
        for content in (None,b'',b'MINGW_W64_VERSION=actual-observation\r\nLLVM_VERSION=other-observation\r\n'):
            file=llvm/'versions.txt'
            if content is not None:file.write_bytes(content)
            for role in ROLES:
                data=package_build_metadata(llvm,role,ctx,library);check_package_metadata(data,role,ctx,str(llvm),library)
                for field,value in [('compiler_archive_sha256','WRONG'),('role','WRONG'),('selected_pthread_sha256','WRONG'),('source_attestation','verified-source')]:
                    bad=copy.deepcopy(data);bad[field]=value
                    try:check_package_metadata(bad,role,ctx,str(llvm),library)
                    except AssertionError:negative+=1
                    else:raise AssertionError('Wrong package context/attestation accepted')
                if content is not None:
                    bad=copy.deepcopy(data);bad['versions']['text']+='tamper'
                    try:check_package_metadata(bad,role,ctx,str(llvm),library)
                    except AssertionError:negative+=1
                    else:raise AssertionError('Tampered package metadata accepted')
    print('PASS package metadata source-only missing/empty/present observations; context/hash/attestation negatives='+str(negative)+'; no native/network execution')

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
    package=package_build_metadata(llvm,role,ctx,sha(lib))
    check_package_metadata(package,role,ctx,str(llvm),sha(lib))
    (dist/'package-metadata.json').write_text(json.dumps(package,sort_keys=True,indent=2))
    print('CPP_PACKAGE_METADATA role='+role+' '+json.dumps(package,sort_keys=True),flush=True)
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
    check_owner_symbols(role,symbols)
    print('CPP_TLS_OWNER_SYMBOLS role='+role+' present='+str(4 if role=='deferred' else 0),flush=True)
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
    expected={'provider.json','package-metadata.json','late-cpp.exe','imports.txt','tls-symbols.txt','link.txt','late-cpp.map'}|{Path(n).stem+'.obj' for n in SOURCES[:3]}
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
    check_package_metadata(json.loads((dist/'package-metadata.json').read_text()),role,ctx,data['compiler_root'],data['library_sha256'])
    provider=json.loads((dist/'provider.json').read_text())
    if role=='published':assert provider==dict(variant=variant,archive=ARCHIVE,cxx=data['driver_sha256'],library=data['library_sha256'],dll=data['dll_sha256'])
    else:
        assert provider['role']==variant and all(provider[k]==v for k,v in ctx.items())
        assert provider['compiler_archive_sha256']==ARCHIVE and provider['cxx_driver_sha256']==data['driver_sha256']
        assert provider['files']['lib/libwinpthread.a']==data['library_sha256'] and provider['files']['bin/libwinpthread-1.dll']==data['dll_sha256']
        assert provider['files']['lib/libpthread.a']==data['pthread_archive_inputs']['libpthread.a']['sha256']
    check_owner_symbols(role,(dist/'tls-symbols.txt').read_text())
    imports=(dist/'imports.txt').read_text().lower();assert 'libwinpthread-1.dll' not in imports and 'libc++' not in imports
    print('Verified distinct C++ TLS variant '+role+' producer='+producer+' same source/run/attempt and complete members',flush=True)
if __name__=='__main__':
    if sys.argv[1]=='selftest':
        selftest_trace()
        selftest_package_metadata()
        selftest_phase_observations()
    elif sys.argv[1]=='build':build(Path(sys.argv[2]),sys.argv[3])
    elif sys.argv[1]=='verify':verify(sys.argv[2])
    elif sys.argv[1]=='observations':
        print('CPP_PHASE_OBSERVATIONS '+json.dumps(check_phase_observations(sys.argv[2],Path(sys.argv[3]).read_text(encoding='utf-8-sig')),sort_keys=True))
    elif sys.argv[1]=='trace':
        check_trace(sys.argv[2],Path(sys.argv[3]).read_text(encoding='utf-8-sig'))
        print('Verified actual C++ identity/join/owned-resource trace role='+sys.argv[2])
    else:raise SystemExit(2)
