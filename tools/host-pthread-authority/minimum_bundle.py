"""CI-only minimum candidate controls; original reference remains a distinct role."""
import json
import os
from pathlib import Path
import posixpath
import re
import shutil
import hashlib
import struct
import base64
import subprocess
import sys

import late_cpp_bundle as cpp
import minimum_receipt as receipt

ROOT = Path(__file__).resolve().parents[2]
DIST = ROOT / 'minimum-controls'
SOURCE = ROOT / 'tests/native/host_pthread_minimum_controls.cc'
VARIANTS = {'reference': 'publisher-reference-unmodified-v1',
            'static': receipt.ROLE, 'dynamic': receipt.ROLE}


def sha(path):
    return receipt.sha(path)


def normal(path):
    return posixpath.normpath(path.replace('\\', '/'))


def loaded_inputs(link):
    origins = {}
    for path in re.findall(r'\bReading ([^\r\n]+\.a)\r?$', link, re.MULTILINE):
        path = normal(path)
        if posixpath.isabs(path):
            origins.setdefault(posixpath.basename(path), set()).add(path)
    rows = set()
    for archive, member, reason in re.findall(r'Loaded ([^\r\n]+?\.a)\(([^()\r\n]+)\) for ([^\r\n]+)', link):
        archive = normal(archive)
        if not posixpath.isabs(archive):
            matches = origins.get(archive, set())
            if archive in ('libwinpthread.a','libpthread.a','libwinpthread.dll.a','libpthread.dll.a'):
                assert len(matches) == 1, 'Unresolved actual pthread import/archive origin: ' + archive
            if len(matches) == 1:
                archive = next(iter(matches))
        rows.add((archive, member, reason))
    return rows


def static_bindings(map_text, link_text, inputs, required):
    definitions = receipt.map_symbols(map_text, required)
    allowed = {normal(value['path']): value['sha256'] for value in inputs.values()}
    loaded = loaded_inputs(link_text)
    result = {}
    for name, (owner, rva) in definitions.items():
        assert owner
        matches = [(archive, member, reason) for archive, member, reason in loaded
                   if posixpath.basename(normal(member)) == posixpath.basename(normal(owner))]
        assert len(matches) == 1, 'Ambiguous/non-selected static member: ' + name
        archive, member, reason = matches[0]
        assert archive in allowed, 'Another supplier filled the minimum static image: ' + name
        result[name] = dict(archive=archive, archive_sha256=allowed[archive], member=member,
                            extracted_for=reason, map_input=owner, map_rva=rva)
    return result


def import_symbols(import_text):
    blocks = {}
    for block in re.findall(r'^Import \{(.*?)^\}', import_text, re.DOTALL | re.MULTILINE):
        names = re.findall(r'^\s*Name: (\S+)\s*$', block, re.MULTILINE)
        assert len(names) == 1
        name = names[0].casefold()
        assert name not in blocks
        blocks[name] = re.findall(r'^\s*Symbol: ([^ (\r\n]+)', block, re.MULTILINE)
    assert blocks, 'No actual COFF import records'
    return blocks


def requested_symbols(nm_text):
    names = re.findall(r'^\s*U (\S+)\s*$', nm_text, re.MULTILINE)
    return sorted({n for n in names if re.fullmatch(r'(?:__imp_)?(?:_?pthread_\w+|__pth_gpointer_locked)', n)})


def short_import_record(body, name, offset):
    assert len(body)>=20
    sig1,sig2,version,machine,timestamp,size,hint,flags=struct.unpack('<HHHHIIHH',body[:20])
    assert sig1==0 and sig2==65535 and version==0 and machine==0x8664
    assert flags>>5==0 and (flags&3) in (0,1,2) and ((flags>>2)&7)==1, 'Unsupported import name encoding'
    assert len(body)==20+size
    strings=body[20:].split(b'\0');assert len(strings)==3 and strings[-1]==b''
    symbol,dll=(s.decode('ascii') for s in strings[:2]);assert symbol and dll and name==dll
    return symbol,dict(member=name,offset=offset,bytes=len(body),sha256=hashlib.sha256(body).hexdigest(),
                        machine=machine,type=flags&3,name_type=(flags>>2)&7,dll=dll,
                        payload_base64=base64.b64encode(body).decode('ascii'))


def short_import_members(payload):
    """Read real COFF archive short imports; never manufacture LLD map rows."""
    assert payload.startswith(b'!<arch>\n'), 'Expected ordinary COFF archive'
    entries=[];offset=8;longnames=None
    while offset<len(payload):
        assert offset+60<=len(payload)
        header=payload[offset:offset+60];assert header[58:]==b'`\n'
        size=int(header[48:58].decode('ascii').strip());assert size>=0
        end=offset+60+size;assert end<=len(payload)
        name=header[:16].decode('ascii').strip();body=payload[offset+60:end]
        entries.append((offset,name,body))
        if name=='//':assert longnames is None;longnames=body
        if size%2:assert payload[end:end+1]==b'\n';end+=1
        offset=end
    assert offset==len(payload)
    result={}
    for offset,name,body in entries:
        if name in ('/','//','/SYM64/'):continue
        if re.fullmatch(r'/[0-9]+',name):
            index=int(name[1:]);assert longnames is not None and index<len(longnames)
            tail=longnames[index:];ends=[n for n in (tail.find(b'\0'),tail.find(b'/\n')) if n>=0]
            assert ends;name=tail[:min(ends)].decode('ascii')
        else:
            assert not name.startswith('#1/');name=name.removesuffix('/')
        if not body.startswith(b'\x00\x00\xff\xff'):continue
        symbol,record=short_import_record(body,name,offset)
        assert symbol not in result,'Ambiguous short-import API'
        result[symbol]=record
    assert result,'No actual short-import members'
    return result


def pe_import_thunks(payload, symbols, requested, diagnostic=False):
    """Bind final x64 function JMPs to the exact named DLL IAT entries."""
    assert payload[:2]==b'MZ'
    pe=struct.unpack_from('<I',payload,0x3c)[0]
    assert payload[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',payload,pe+4)[0]==0x8664
    count=struct.unpack_from('<H',payload,pe+6)[0];optional_size=struct.unpack_from('<H',payload,pe+20)[0]
    optional=pe+24;assert struct.unpack_from('<H',payload,optional)[0]==0x20b
    base=struct.unpack_from('<Q',payload,optional+24)[0]
    assert struct.unpack_from('<I',payload,optional+108)[0]>1
    import_rva,import_size=struct.unpack_from('<II',payload,optional+120)
    sections=[]
    for i in range(count):
        at=optional+optional_size+i*40
        virtual_size,rva,size,offset=struct.unpack_from('<IIII',payload,at+8)
        assert offset+size<=len(payload);sections.append((rva,size,offset))
    def location(rva,size=1):
        matches=[off+rva-start for start,rawsize,off in sections if start<=rva and rva+size<=start+rawsize]
        assert len(matches)==1,'Unmapped/ambiguous PE RVA'
        return matches[0]
    def cstring(rva):
        start=location(rva);end=payload.find(b'\0',start);assert end>=start
        location(rva,end-start+1);return payload[start:end].decode('ascii')
    imports={};finished=False
    for i in range(import_size//20):
        desc=struct.unpack_from('<IIIII',payload,location(import_rva+i*20,20))
        if not any(desc):finished=True;break
        lookup,stamp,chain,name,iat=desc;dll=cstring(name).casefold()
        assert lookup and iat
        for j in range(len(payload)//8):
            item=struct.unpack_from('<Q',payload,location(lookup+j*8,8))[0]
            if item==0:break
            if item>>63:continue
            api=cstring(item+2);key=(dll,api);assert key not in imports
            imports[key]=base+iat+j*8
        else:raise AssertionError('Unterminated PE import lookup')
    assert finished
    definitions={};needed={s.removeprefix('__imp_') for s in requested}
    needed|={'__imp_'+s for s in needed}
    for address,kind,name in re.findall(r'(?m)^([0-9a-fA-F]+) ([A-Za-z?]) (\S+)\r?$',symbols):
        if name not in needed:continue
        value=int(address,16);assert name not in definitions or definitions[name]==value
        definitions[name]=value
    result={}
    for symbol in requested:
        api=symbol.removeprefix('__imp_');iat=imports[('libwinpthread-1.dll',api)]
        assert not any(name==api and dll!='libwinpthread-1.dll' for dll,name in imports)
        assert definitions['__imp_'+api]==iat
        record=dict(iat_va=iat,dll='libwinpthread-1.dll',api=api)
        if not symbol.startswith('__imp_'):
            address=definitions[api];offset=location(address-base,6);code=payload[offset:offset+6]
            if diagnostic:
                print('MINIMUM_RAW_PE_THUNK symbol='+symbol+' '+json.dumps(dict(image_base=base,thunk_va=address,iat_va=iat,bytes=code.hex()),sort_keys=True),flush=True)
            assert code[:2]==b'\xff\x25','Requested API resolved to static code instead of import thunk'
            assert address+6+struct.unpack('<i',code[2:])[0]==iat,'Import thunk targets another IAT slot'
            record.update(thunk_va=address,thunk_bytes=code.hex())
        result[symbol]=record
    return result


def short_import_input(link_text, inputs, count):
    allowed = {normal(value['path']): value['sha256'] for value in inputs.values()}
    origins={}
    for path in re.findall(r'\bReading ([^\r\n]+\.a)\r?$',link_text,re.MULTILINE):
        path=normal(path)
        if posixpath.isabs(path):origins.setdefault(posixpath.basename(path),set()).add(path)
    observed=[]
    for archive,member in re.findall(r'\bReading ([^\r\n]+?\.a)\(([^()\r\n]+)\)\r?$',link_text,re.MULTILINE):
        if member.casefold()!='libwinpthread-1.dll':continue
        archive=normal(archive)
        if not posixpath.isabs(archive):
            matches=origins.get(archive,set());assert len(matches)==1,'Unresolved actual short-import archive'
            archive=next(iter(matches))
        assert archive in allowed,'Another archive supplied pthread short imports'
        observed.append((archive,member))
    assert len(observed)==count and len(set(observed))==1,'Missing/ambiguous actual short-import input census'
    archive,member=observed[0]
    return archive,member,len(observed)


def dynamic_bindings(link_text, inputs, imports_text, requested, members, nm_text, thunks):
    blocks = import_symbols(imports_text)
    assert 'libwinpthread-1.dll' in blocks
    allowed = {normal(value['path']): value['sha256'] for value in inputs.values()}
    apis={s.removeprefix('__imp_') for s in requested}
    archive,member,observed_count=short_import_input(link_text,inputs,len(apis))
    assert len({r['offset'] for r in members.values()})==len(members)
    for api,record in members.items():
        name,reconstructed=short_import_record(base64.b64decode(record['payload_base64'],validate=True),record['member'],record['offset'])
        assert name==api and reconstructed==record,'Short-import payload/record mismatch'
    nm_records=short_import_nm(nm_text,apis)
    result = {}
    for symbol in requested:
        api = symbol.removeprefix('__imp_')
        assert api in blocks['libwinpthread-1.dll'], 'Requested API not imported from selected DLL: ' + api
        assert not any(api in names for dll, names in blocks.items() if dll != 'libwinpthread-1.dll')
        record=members[api]
        assert record['member']==record['dll']==member and record['machine']==0x8664 and record['type']==0 and record['name_type']==1
        assert record['offset']>=8 and record['bytes']>20 and re.fullmatch('[0-9a-f]{64}',record['sha256'])
        thunk=thunks[symbol];assert thunk['api']==api and thunk['dll']=='libwinpthread-1.dll'
        result[symbol] = dict(api=api, dll='libwinpthread-1.dll', archive=archive,
                              archive_sha256=allowed[archive], member=member,
                              short_import=record,iat_api=api,actual_member_read_count=observed_count,
                              final_pe_thunk=thunk,
                              evidence='actual-short-import-input-and-final-IAT; no regular-map row exists')
    return result


def short_import_nm(nm_text, apis):
    nm_records={}
    for member,block in re.findall(r'(?ms)^([^\r\n]+):\r?\n(.*?)(?=^[^\r\n]+:\r?\n|\Z)',nm_text):
        names=re.findall(r'(?m)^00000000 T (\S+)\r?$',block)
        for api in apis:
            if api in names:
                assert member=='libwinpthread-1.dll','Import nm member differs from short payload DLL'
                assert api not in nm_records
                nm_records[api]=set(names)
    for api in apis:
        assert nm_records[api]=={api,'__imp_'+api},'Missing/ambiguous actual import nm pair'
    return nm_records


def check_supplier(data, ctx):
    assert data['role'] == receipt.ROLE and data['pin'] == receipt.PIN
    assert all(data[k] == v for k, v in ctx.items())
    assert data['source_files'] == receipt.PIN['output_source_sha256']
    assert data['compiler_archive_sha256'] == receipt.PIN['compiler_archive_sha256']
    receipt.validate_layout(data['private_record_layout'])
    assert data['original']['context'] == ctx and data['original']['role'] == VARIANTS['reference']
    assert data['public_headers'] == data['original']['public_headers']
    assert data['public_headers']['pthread.h'] == receipt.PIN['source_sha256']['mingw-w64-libraries/winpthreads/include/pthread.h']
    assert receipt.exports(data['dll_exports']) == data['original']['dll_exports']
    assert receipt.imports(data['dll_imports']) == data['original']['dll_import_names']
    assert set(data['dll_symbol_objects']) == set(receipt.CORE)
    assert all(re.fullmatch('[0-9a-f]{64}', row['sha256']) for row in data['dll_symbol_objects'].values())
    assert len({row['actual_reading_path'] for row in data['dll_symbol_objects'].values()}) == 1
    for raw in data['library_symbols'].values():
        assert 'gesher_' not in raw.lower()
    assert data['original']['files']['lib/libwinpthread.a'] == receipt.PIN['publisher_static_sha256']
    assert data['original_profile']['sdk'] == data['original_profile']['supplier']
    assert data['original_profile']['supplier']['_WIN32_WINNT'] == 0x0601
    assert data['files']['lib/libwinpthread.a'] == data['files']['lib/libpthread.a']
    assert data['files']['lib/libwinpthread.dll.a'] == data['files']['lib/libpthread.dll.a']


def errno_values(text):
    macros=dict(re.findall(r'^#define ([A-Za-z_][A-Za-z_0-9]*) (.+)$',text,re.MULTILINE))
    result={}
    for name in ('EDEADLK','ESRCH','EINVAL','EBUSY'):
        value=macros[name];seen=set()
        while value in macros:
            assert value not in seen;seen.add(value);value=macros[value]
        assert re.fullmatch(r'0[xX][0-9a-fA-F]+|[0-9]+',value)
        result[name]=int(value,0);assert result[name]>0
    assert len(set(result.values()))==4
    return result


def check_control_trace(tag,mode,text,errno):
    assert tag in VARIANTS and mode in ('attachment','contracts')
    rows={};positions={}
    for number,line in enumerate(text.splitlines()):
        if not line.startswith('MINIMUM_'):continue
        words=line.split();label=words[0];record={}
        for word in words[1:]:
            assert word.count('=')==1,'Malformed minimum trace field'
            key,value=word.split('=');assert key not in record;record[key]=value
        rows.setdefault(label,[]).append(record);positions.setdefault(label,[]).append(number)
    assert 'MINIMUM_ASSERT_FAIL' not in rows and 'MINIMUM_SETUP_FAIL' not in rows
    def one(label):
        values=rows.get(label,[]);assert len(values)==1,'Missing/ambiguous minimum row: '+label
        return values[0]
    def integers(row):
        assert all(re.fullmatch(r'-?[0-9]+',str(v)) for v in row.values())
        return {k:int(v) for k,v in row.items()}
    provider=integers(one('MINIMUM_PROVIDER'))
    assert provider==({'dll':1,'exact_executable_directory':1} if tag=='dynamic' else {'dll':0})
    if mode=='attachment':
        row=integers(one('MINIMUM_ATTACHMENT'));expected=0 if tag=='reference' else 2
        assert set(rows)=={'MINIMUM_PROVIDER','MINIMUM_ATTACHMENT'}
        assert row['expected_delta']==row['delta']==expected and row['repeated_delta']==0
        assert row['after_first_self']-row['before_first_self']==expected and row['after_second_self']==row['after_first_self']
        assert row['first']==row['second'] and row['first']>0 and row['native_tid']>0
        assert row['observation_only']==row['original_census_unchanged']==1
        assert all(row[k]>=0 for k in ('cold','after_key_setup','before_first_self','after_first_self','after_second_self'))
        return
    assert tag!='reference','Original control is not silently promoted to minimum conformance'
    expected_labels={'MINIMUM_PROVIDER','MINIMUM_SELF_BEFORE','MINIMUM_SELF_RETURN','MINIMUM_OWNED_SELF','MINIMUM_FOREIGN_SELF','MINIMUM_ERROR','MINIMUM_INVALID_ATTR','MINIMUM_DETACHED','MINIMUM_TERMINATED_OBJECT','MINIMUM_HELD_HELPER_READY','MINIMUM_HELD_BEFORE','MINIMUM_HELD_TRYJOIN','MINIMUM_HELD_AFTER','MINIMUM_CONTRACTS_COMPLETE'}
    assert set(rows)==expected_labels
    owned=integers(one('MINIMUM_OWNED_SELF'));foreign=integers(one('MINIMUM_FOREIGN_SELF'))
    assert owned['id']>0 and foreign['id']>0 and owned['id']!=foreign['id']
    assert owned['native_tid']>0 and owned['borrowed_kernel']==258
    assert owned['join_self']==owned['try_self']==errno['EDEADLK'] and owned['later_join']==0
    assert owned['actual_kernel']==0 and owned['native_exit']==4660 and owned['return_match']==1
    assert foreign['native_tid']==foreign['borrowed_tid'] and foreign['native_tid']>0 and foreign['borrowed_kernel']==258
    assert foreign['join_self']==foreign['try_self']==errno['EDEADLK'] and foreign['native_exit']==61
    assert foreign['join_output_unchanged']==foreign['try_output_unchanged']==1
    before={};returned={}
    assert len(rows['MINIMUM_SELF_BEFORE'])==len(rows['MINIMUM_SELF_RETURN'])==2
    for raw in rows['MINIMUM_SELF_BEFORE']:
        row=integers(raw);assert row['id'] not in before;before[row['id']]=row
        assert row['native_tid']==row['borrowed_tid'] and row['native_tid']>0 and row['kernel']==258
    for raw in rows['MINIMUM_SELF_RETURN']:
        row=integers(raw);assert row['id'] not in returned;returned[row['id']]=row
        assert row['join_rc']==row['try_rc']==row['expected']==errno['EDEADLK']
        assert row['join_output_unchanged']==row['try_output_unchanged']==1
    assert set(before)==set(returned)=={owned['id'],foreign['id']}
    assert before[owned['id']]['native_tid']==owned['native_tid'] and before[foreign['id']]['native_tid']==foreign['native_tid']
    errors={}
    for raw in rows['MINIMUM_ERROR']:
        row=dict(raw);label=row.pop('label');assert label not in errors;row=integers(row);errors[label]=row
        assert row['join_rc']==row['try_rc']==row['expected']==errno['ESRCH']
        assert row['join_output_unchanged']==row['try_output_unchanged']==1
    assert set(errors)=={'invalid_zero','invalid_max','stale_owned','stale_foreign_after_kernel','attr_detached','detach_live'}
    assert errors['invalid_zero']['id']==0 and errors['invalid_max']['id']==18446744073709551615
    assert errors['stale_owned']['id']==owned['id'] and errors['stale_foreign_after_kernel']['id']==foreign['id']
    attr=integers(one('MINIMUM_INVALID_ATTR'));assert attr==dict(rc=errno['EINVAL'],expected=errno['EINVAL'],detached_valid_handle_not_fabricated=1)
    detached={}
    for raw in rows['MINIMUM_DETACHED']:
        row=dict(raw);label=row.pop('label');assert label not in detached;row=integers(row);detached[label]=row
        assert row['id']==errors[label]['id'] and row['id']>0 and row['native_tid']>0
        assert row['public_handle_null']==1 and row['independent_kernel']==258
        assert row['repeat_detach']==row['expected']==errno['ESRCH']
    assert set(detached)=={'attr_detached','detach_live'}
    terminated=integers(one('MINIMUM_TERMINATED_OBJECT'))
    assert terminated['old_tid']==terminated['actual_object_tid'] and terminated['old_tid']>0 and terminated['actual_current_tid']>0
    assert terminated['kernel']==0 and terminated['would_be_self']==0 and terminated['optional_reuse_only']==1
    assert terminated['os_tid_reuse_observed']==int(terminated['actual_object_tid']==terminated['actual_current_tid'])
    assert integers(one('MINIMUM_HELD_HELPER_READY'))==dict(before_target=1,native_only=1)
    held=integers(one('MINIMUM_HELD_BEFORE'))
    assert held['id']==held['record_id'] and held['id']>0 and held['ended']==1
    assert held['stored_tid']==held['actual_tid']==held['borrowed_tid'] and held['actual_tid']>0
    assert held['kernel']==258 and held['cpp_constructed']==1 and held['cpp_returned']==0 and held['private_size']==480
    attempt=integers(one('MINIMUM_HELD_TRYJOIN'))
    assert attempt==dict(rc=errno['EBUSY'],expected=errno['EBUSY'],result_unchanged=1,result_return=0,kernel_after=258,consumed=0)
    after=integers(one('MINIMUM_HELD_AFTER'))
    assert after==dict(actual_kernel=0,cpp_returned=1,helper_kernel=0,helper_exit=0,target_exit=4660,normal_join=0,normal_return=1,consumed_id_rejoined=0)
    assert positions['MINIMUM_HELD_HELPER_READY'][0]<positions['MINIMUM_HELD_BEFORE'][0]<positions['MINIMUM_HELD_TRYJOIN'][0]<positions['MINIMUM_HELD_AFTER'][0]
    assert integers(one('MINIMUM_CONTRACTS_COMPLETE'))==dict(failures=0,original_census_unchanged=1,lifecycle_adoption_not_claimed=1)


def build(reference_llvm, minimum_llvm, source):
    ctx = receipt.context()
    assert os.environ['GITHUB_JOB'] == 'minimum_compile'
    supplier = json.loads((minimum_llvm / 'gesher-winpthreads-authority.json').read_text())
    check_supplier(supplier, ctx)
    assert not (reference_llvm / 'gesher-winpthreads-authority.json').exists()
    for llvm in (reference_llvm, minimum_llvm):
        assert sha(llvm.parent / 'compiler.tar.xz') == receipt.PIN['compiler_archive_sha256']
    for name, digest in supplier['original']['files'].items():
        assert sha(reference_llvm / 'x86_64-w64-mingw32' / name) == digest
    DIST.mkdir()
    (DIST / 'minimum-supplier.json').write_bytes((minimum_llvm / 'gesher-winpthreads-authority.json').read_bytes())
    (DIST / 'reference-supplier.json').write_text(json.dumps(supplier['original'], sort_keys=True, indent=2))
    shutil.copyfile(minimum_llvm / 'x86_64-w64-mingw32/bin/libwinpthread-1.dll', DIST / 'libwinpthread-1.dll')
    include = source / 'mingw-w64-libraries/winpthreads/src'
    private_headers = {name: dict(path=str(include / name), sha256=sha(include / name)) for name in ('thread.h', 'rwlock.h')}
    for name, record in private_headers.items():
        assert record['sha256'] == receipt.PIN['source_sha256']['mingw-w64-libraries/winpthreads/src/' + name]
    variants = {}
    for tag in VARIANTS:
        dynamic = tag == 'dynamic'
        llvm = reference_llvm if tag == 'reference' else minimum_llvm
        provider = supplier['original'] if tag == 'reference' else supplier
        driver = llvm / 'bin/x86_64-w64-mingw32-clang++'
        assert sha(driver) == provider['cxx_driver_sha256']
        target = llvm / 'x86_64-w64-mingw32'
        macro_argv=[str(driver),'-dM','-E','-x','c++']+[flag for flag in cpp.FLAGS if flag.startswith('-D')]+['-']
        macro_text=subprocess.check_output(macro_argv,input='#include <pthread.h>\n',text=True)
        (DIST/(tag+'-errno-macros.txt')).write_text(macro_text)
        errno=errno_values(macro_text)
        print('MINIMUM_ERRNO_PROFILE tag='+tag+' '+json.dumps(errno,sort_keys=True),flush=True)
        obj = DIST / (tag + '.obj')
        compile_argv = [str(driver)] + cpp.FLAGS + ['-I', str(include), '-c', str(SOURCE), '-o', str(obj)]
        subprocess.check_call(compile_argv)
        undefined = subprocess.check_output([str(llvm / 'bin/llvm-nm'), '--undefined-only', str(obj)], text=True)
        (DIST / (tag + '-undefined.txt')).write_text(undefined)
        print('MINIMUM_CONTROL_UNDEFINED tag='+tag+'\n'+undefined,flush=True)
        requested = requested_symbols(undefined)
        assert {'pthread_self', 'pthread_create', 'pthread_join', '_pthread_tryjoin', '__pth_gpointer_locked'} <= {n.removeprefix('__imp_') for n in requested}
        archive_name = 'libwinpthread.dll.a' if dynamic else 'libwinpthread.a'
        library = target / 'lib' / archive_name
        assert sha(library) == provider['files']['lib/' + archive_name]
        names = ('libwinpthread.dll.a', 'libpthread.dll.a') if dynamic else ('libwinpthread.a', 'libpthread.a')
        inputs = {name: dict(path=str(target / 'lib' / name), sha256=sha(target / 'lib' / name)) for name in names}
        assert len({r['sha256'] for r in inputs.values()}) == 1
        output = DIST / (tag + '.exe')
        link_argv = [str(driver), '-static'] + ([] if dynamic else ['-pthread']) + [str(obj), str(library), '-Wl,--verbose', '-Wl,-Map,' + str(DIST / (tag + '.map')), '-o', str(output)]
        link = subprocess.run(link_argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(link.stdout, end='', flush=True)
        (DIST / (tag + '-link.txt')).write_text(link.stdout)
        link.check_returncode()
        import_text = subprocess.check_output([str(llvm / 'bin/llvm-readobj'), '--coff-imports', str(output)], text=True)
        symbols = subprocess.check_output([str(llvm / 'bin/llvm-nm'), '--defined-only', str(output)], text=True)
        machine = subprocess.check_output([str(llvm / 'bin/llvm-readobj'), '--file-headers', str(output)], text=True)
        assert 'IMAGE_FILE_MACHINE_AMD64' in machine
        assert ('libwinpthread-1.dll' in import_text.lower()) == dynamic
        assert 'libc++' not in import_text.lower()
        assert 'gesher_' not in symbols.lower()
        (DIST / (tag + '-imports.txt')).write_text(import_text)
        (DIST / (tag + '-symbols.txt')).write_text(symbols)
        (DIST / (tag + '-machine.txt')).write_text(machine)
        map_text = (DIST / (tag + '.map')).read_text()
        print('MINIMUM_CONTROL_RAW_MAP_BEGIN tag=' + tag + '\n' + map_text + 'MINIMUM_CONTROL_RAW_MAP_END', flush=True)
        print('MINIMUM_CONTROL_RAW_IMPORTS_BEGIN tag='+tag+'\n'+import_text+'MINIMUM_CONTROL_RAW_IMPORTS_END',flush=True)
        print('MINIMUM_CONTROL_RAW_SYMBOLS_BEGIN tag='+tag+'\n'+symbols+'MINIMUM_CONTROL_RAW_SYMBOLS_END',flush=True)
        if dynamic:
            assert not re.search(r'\b(?:__dyn_tls_pthread|__xl_f)\s*$', symbols, re.MULTILINE), 'Static pthread provider leaked into import consumer'
            import_members=short_import_members(library.read_bytes())
            (DIST/'dynamic-short-import-members.json').write_text(json.dumps(import_members,sort_keys=True,indent=2))
            for api in sorted({n.removeprefix('__imp_') for n in requested}):
                print('MINIMUM_ACTUAL_SHORT_IMPORT api='+api+' '+json.dumps(import_members[api],sort_keys=True),flush=True)
            thunks=pe_import_thunks(output.read_bytes(),symbols,requested,diagnostic=True)
            for name,record in thunks.items():print('MINIMUM_FINAL_PE_IMPORT symbol='+name+' '+json.dumps(record,sort_keys=True),flush=True)
            bindings = dynamic_bindings(link.stdout, inputs, import_text, requested, import_members,
                                        supplier['library_symbols']['lib/libwinpthread.dll.a'],thunks)
        else:
            bindings = static_bindings(map_text, link.stdout, inputs, sorted(set(cpp.PTHREAD_SYMBOLS) | set(requested)))
        for name, binding in bindings.items():
            print('MINIMUM_CONTROL_MEMBER tag=' + tag + ' symbol=' + name + ' ' + json.dumps(binding, sort_keys=True), flush=True)
        variants[tag] = dict(role=VARIANTS[tag], compiler_root=str(llvm), driver=str(driver),
                             cxx_driver_sha256=sha(driver), compile=compile_argv, link=link_argv, errno=errno, errno_command=macro_argv,
                             requested=requested, pthread_archive_inputs=inputs, symbol_bindings=bindings)
        print('MINIMUM_CONTROL_BINARY tag=' + tag + ' role=' + VARIANTS[tag] + ' exe=' + sha(output) + ' object=' + sha(obj) + ' library=' + sha(library), flush=True)
    data = dict(context=ctx, role='minimum-candidate-controls-v1', producer='minimum_compile',
                workspace=str(ROOT), source=str(SOURCE), source_sha256=sha(SOURCE),
                private_headers=private_headers, supplier_source_root=str(source), variants=variants,
                members={p.name: sha(p) for p in DIST.iterdir()})
    (DIST / 'bundle.json').write_text(json.dumps(data, sort_keys=True, indent=2))


def verify():
    ctx = receipt.context()
    data = json.loads((DIST / 'bundle.json').read_text())
    assert data['context'] == ctx and data['role'] == 'minimum-candidate-controls-v1' and data['producer'] == 'minimum_compile'
    expected = {'minimum-supplier.json', 'reference-supplier.json', 'libwinpthread-1.dll','dynamic-short-import-members.json'}
    expected |= {tag + suffix for tag in VARIANTS for suffix in ('.exe', '.obj', '.map', '-link.txt', '-undefined.txt', '-imports.txt', '-symbols.txt', '-machine.txt', '-errno-macros.txt')}
    assert set(data['members']) == expected and {p.name for p in DIST.iterdir()} == expected | {'bundle.json'}
    assert all(sha(DIST / name) == digest for name, digest in data['members'].items())
    assert data['source_sha256'] == sha(SOURCE) and data['source'] == data['workspace'] + '/tests/native/host_pthread_minimum_controls.cc'
    assert data['private_headers'] == {name: dict(path=data['supplier_source_root'] + '/mingw-w64-libraries/winpthreads/src/' + name,
            sha256=receipt.PIN['source_sha256']['mingw-w64-libraries/winpthreads/src/' + name]) for name in ('thread.h', 'rwlock.h')}
    supplier = json.loads((DIST / 'minimum-supplier.json').read_text())
    check_supplier(supplier, ctx)
    original = json.loads((DIST / 'reference-supplier.json').read_text())
    assert original == supplier['original']
    assert sha(DIST / 'libwinpthread-1.dll') == supplier['files']['bin/libwinpthread-1.dll']
    assert set(data['variants']) == set(VARIANTS)
    for tag, row in data['variants'].items():
        dynamic = tag == 'dynamic'
        provider = original if tag == 'reference' else supplier
        assert row['role'] == VARIANTS[tag] and row['cxx_driver_sha256'] == provider['cxx_driver_sha256']
        assert row['driver'] == row['compiler_root'] + '/bin/x86_64-w64-mingw32-clang++'
        assert row['errno_command']==[row['driver'],'-dM','-E','-x','c++']+[flag for flag in cpp.FLAGS if flag.startswith('-D')]+['-']
        assert row['errno']==errno_values((DIST/(tag+'-errno-macros.txt')).read_text())
        expected_compile = [row['driver']] + cpp.FLAGS + ['-I', data['supplier_source_root'] + '/mingw-w64-libraries/winpthreads/src', '-c', data['source'], '-o', data['workspace'] + '/minimum-controls/' + tag + '.obj']
        assert row['compile'] == expected_compile
        archive = 'libwinpthread.dll.a' if dynamic else 'libwinpthread.a'
        expected_link = [row['driver'], '-static'] + ([] if dynamic else ['-pthread']) + [data['workspace'] + '/minimum-controls/' + tag + '.obj', row['compiler_root'] + '/x86_64-w64-mingw32/lib/' + archive,
                '-Wl,--verbose', '-Wl,-Map,' + data['workspace'] + '/minimum-controls/' + tag + '.map', '-o', data['workspace'] + '/minimum-controls/' + tag + '.exe']
        assert row['link'] == expected_link
        names = ('libwinpthread.dll.a', 'libpthread.dll.a') if dynamic else ('libwinpthread.a', 'libpthread.a')
        assert row['pthread_archive_inputs'] == {name: dict(path=row['compiler_root'] + '/x86_64-w64-mingw32/lib/' + name, sha256=provider['files']['lib/' + name]) for name in names}
        requested = requested_symbols((DIST / (tag + '-undefined.txt')).read_text())
        assert row['requested'] == requested
        map_text = (DIST / (tag + '.map')).read_text()
        link_text = (DIST / (tag + '-link.txt')).read_text()
        raw_imports = (DIST / (tag + '-imports.txt')).read_text()
        raw_symbols = (DIST / (tag + '-symbols.txt')).read_text()
        assert ('libwinpthread-1.dll' in raw_imports.lower()) == dynamic and 'libc++' not in raw_imports.lower()
        assert 'IMAGE_FILE_MACHINE_AMD64' in (DIST / (tag + '-machine.txt')).read_text()
        assert 'gesher_' not in raw_symbols.lower()
        if dynamic:
            assert not re.search(r'\b(?:__dyn_tls_pthread|__xl_f)\s*$', raw_symbols, re.MULTILINE)
            import_members=json.loads((DIST/'dynamic-short-import-members.json').read_text())
            thunks=pe_import_thunks((DIST/(tag+'.exe')).read_bytes(),raw_symbols,requested,diagnostic=True)
            bindings = dynamic_bindings(link_text,row['pthread_archive_inputs'],raw_imports,requested,
                            import_members,supplier['library_symbols']['lib/libwinpthread.dll.a'],thunks)
        else:
            bindings = static_bindings(map_text, link_text, row['pthread_archive_inputs'], sorted(set(cpp.PTHREAD_SYMBOLS) | set(requested)))
        assert row['symbol_bindings'] == bindings
    print('Verified distinct original-reference/minimum-static/minimum-import controls; original480/private-read headers and exact current-attempt source/member/command/ZIP context')


def selftest():
    names=['pthread_self','__pth_gpointer_locked']
    inputs={n:dict(path='/minimum/lib/'+n,sha256='1'*64) for n in ('libwinpthread.dll.a','libpthread.dll.a')}
    # Real LLD short imports have Reading rows and no regular symbol map rows.
    link='ld.lld: Reading /minimum/lib/libwinpthread.dll.a\n'+'ld.lld: Reading libwinpthread.dll.a(libwinpthread-1.dll)\n'*len(names)
    imported='Import {\n  Name: libwinpthread-1.dll\n'+''.join('  Symbol: '+n+' (1)\n' for n in names)+'}\n'
    nm=''.join('\nlibwinpthread-1.dll:\n00000000 T '+n+'\n00000000 T __imp_'+n+'\n' for n in names)
    def member(name):
        data=(name+'\0libwinpthread-1.dll\0').encode()
        return struct.pack('<HHHHIIHH',0,65535,0,0x8664,0,len(data),0,4)+data
    def archive(entries):
        payload=b'!<arch>\n'
        for name,body in entries:
            header=(name.ljust(16)+'0'.ljust(12)+'0'.ljust(6)+'0'.ljust(6)+'0'.ljust(8)+str(len(body)).ljust(10)+'`\n').encode()
            assert len(header)==60;payload+=header+body+(b'\n' if len(body)%2 else b'')
        return payload
    binary=archive([('//',b'libwinpthread-1.dll\0')]+[('/0',member(n)) for n in names])
    members=short_import_members(binary)
    thunks={n:dict(iat_va=0x140002100+i*8,dll='libwinpthread-1.dll',api=n,thunk_va=0x140001000+i*16,thunk_bytes='ff2500000000') for i,n in enumerate(names)}
    good=dynamic_bindings(link,inputs,imported,names,members,nm,thunks);assert set(good)==set(names)
    rejected=0
    controls=[(link,'',members,nm),(link,imported.replace('libwinpthread-1.dll','other.dll'),members,nm),
        (link.replace('/minimum/','/other/'),imported,members,nm),
        (link.replace('ld.lld: Reading /minimum/lib/libwinpthread.dll.a\n',''),imported,members,nm),
        (link+'ld.lld: Reading /other/lib/libwinpthread.dll.a\n',imported,members,nm),
        (link+'ld.lld: Reading /other/lib/other.a(libwinpthread-1.dll)\n',imported,members,nm),
        (link.replace('ld.lld: Reading libwinpthread.dll.a(libwinpthread-1.dll)\n','',1),imported,members,nm),
        (link,imported,{n:v for n,v in members.items() if n!=names[0]},nm),
        (link,imported,members,nm.replace('00000000 T __imp_pthread_self\n','')),
        (link,imported,members,nm+nm),
        (link,imported,members,nm.replace('libwinpthread-1.dll:','other.dll:',1))]
    # Mutated record claims cannot substitute for the actual short payload.
    for key,value in [('machine',0x14c),('type',1),('name_type',2),('dll','other.dll'),('member','other.dll'),('bytes',1),('sha256','0'*64),('offset',0),('payload_base64','AAAA')]:
        changed=json.loads(json.dumps(members));changed[names[0]][key]=value
        controls.append((link,imported,changed,nm))
    for altered in controls:
        try:dynamic_bindings(altered[0],inputs,altered[1],names,*altered[2:],thunks)
        except (AssertionError,KeyError,struct.error):rejected+=1
        else:raise AssertionError('Unbound short-import/actual-IAT metadata accepted')
    for corrupt in (binary[:-1],binary.replace(b'!<arch>',b'!<thin>'),binary.replace(b'pthread_self\0',b'pthread_else\0',1)+b'x',archive([('/0',member(names[0]))]),archive([('//',b'libwinpthread-1.dll\0'),('/0',member(names[0])),('/0',member(names[0]))])):
        try:short_import_members(corrupt)
        except (AssertionError,ValueError,struct.error):rejected+=1
        else:raise AssertionError('Malformed/ambiguous short-import archive accepted')
    header='00001000 00000020     4         thread.o:(.text)\n'
    rows={n:'00001000 00000000     0                 '+n+'\n' for n in names}
    static_map=header+''.join(rows.values())
    static_inputs={n:dict(path='/minimum/lib/'+n,sha256='2'*64) for n in ('libwinpthread.a','libpthread.a')}
    static_link='ld.lld: Reading /minimum/lib/libwinpthread.a\nld.lld: Loaded libwinpthread.a(thread.o) for pthread_self\n'
    assert set(static_bindings(static_map,static_link,static_inputs,names))==set(names)
    for extra in ('ld.lld: Loaded /other/lib/other.a(thread.o) for another_symbol\n',):
        try:static_bindings(static_map,static_link+extra,static_inputs,names)
        except AssertionError:rejected+=1
        else:raise AssertionError('Ambiguous static supplier accepted')
    assert requested_symbols('                 U pthread_self\n                 U __pth_gpointer_locked\n                 U __imp__pthread_tryjoin\n                 U unrelated\n')==['__imp__pthread_tryjoin','__pth_gpointer_locked','pthread_self']
    print('PASS minimum actual-short-import/archive/IAT/nm/static negatives='+str(rejected)+'; no compiler/native/network operation')
    selftest_pe()
    selftest_trace()


def selftest_pe():
    # In-memory format metadata only. Never written as an executable or run.
    data=bytearray(0x600);data[:2]=b'MZ';struct.pack_into('<I',data,0x3c,0x80);data[0x80:0x84]=b'PE\0\0'
    struct.pack_into('<HH',data,0x84,0x8664,1);struct.pack_into('<H',data,0x94,240)
    optional=0x98;struct.pack_into('<H',data,optional,0x20b);base=0x140000000
    struct.pack_into('<Q',data,optional+24,base);struct.pack_into('<I',data,optional+108,16)
    struct.pack_into('<II',data,optional+120,0x1100,40)
    struct.pack_into('<IIII',data,optional+240+8,0x400,0x1000,0x400,0x200)
    struct.pack_into('<IIIII',data,0x300,0x1180,0,0,0x1140,0x11a0)
    dll=b'libwinpthread-1.dll\0';data[0x340:0x340+len(dll)]=dll
    api=b'pthread_self\0';data[0x3c2:0x3c2+len(api)]=api
    struct.pack_into('<Q',data,0x380,0x11c0);struct.pack_into('<Q',data,0x3a0,0x11c0)
    data[0x200:0x206]=b'\xff\x25'+struct.pack('<i',0x11a0-0x1000-6)
    symbols='0000000140001000 T pthread_self\n00000001400011a0 I __imp_pthread_self\n'
    result=pe_import_thunks(bytes(data),symbols,['pthread_self','__imp_pthread_self'])
    assert result['pthread_self']['iat_va']==base+0x11a0 and result['pthread_self']['thunk_bytes'].startswith('ff25')
    controls=[]
    for offset,value in ((0,0),(0x200,0x90),(0x202,0),(0x84,0),(0x380,0),(0x340,ord('x'))):
        bad=bytearray(data);bad[offset]=value;controls.append((bytes(bad),symbols))
    controls += [(bytes(data),symbols.replace('1400011a0','1400011b0')),(bytes(data),symbols+'0000000140001010 T pthread_self\n')]
    rejected=0
    for payload,nm in controls:
        try:pe_import_thunks(payload,nm,['pthread_self'])
        except (AssertionError,KeyError,struct.error):rejected+=1
        else:raise AssertionError('Static/wrong-slot/missing/ambiguous final PE import accepted')
    print('PASS final PE x64 thunk/named-IAT metadata negatives='+str(rejected)+'; zero native or disk-executable operations')


def selftest_trace():
    codes=dict(EDEADLK=36,ESRCH=3,EINVAL=22,EBUSY=16)
    assert errno_values('\n'.join('#define '+k+' '+str(v) for k,v in codes.items()))==codes
    lines=['MINIMUM_PROVIDER dll=0']
    for identity,tid in ((11,101),(12,102)):
        lines += [f'MINIMUM_SELF_BEFORE id={identity} native_tid={tid} borrowed_tid={tid} kernel=258',
            f'MINIMUM_SELF_RETURN id={identity} join_rc=36 try_rc=36 expected=36 join_output_unchanged=1 try_output_unchanged=1']
    lines += ['MINIMUM_OWNED_SELF id=11 native_tid=101 borrowed_kernel=258 join_self=36 try_self=36 later_join=0 actual_kernel=0 native_exit=4660 return_match=1',
        'MINIMUM_FOREIGN_SELF id=12 native_tid=102 borrowed_tid=102 borrowed_kernel=258 join_self=36 try_self=36 native_exit=61 join_output_unchanged=1 try_output_unchanged=1']
    for label,identity in (('invalid_zero',0),('invalid_max',18446744073709551615),('stale_owned',11),('stale_foreign_after_kernel',12),('attr_detached',13),('detach_live',14)):
        lines.append(f'MINIMUM_ERROR label={label} id={identity} join_rc=3 try_rc=3 expected=3 join_output_unchanged=1 try_output_unchanged=1')
    lines.append('MINIMUM_INVALID_ATTR rc=22 expected=22 detached_valid_handle_not_fabricated=1')
    for label,identity in (('attr_detached',13),('detach_live',14)):
        lines.append(f'MINIMUM_DETACHED label={label} id={identity} public_handle_null=1 native_tid=103 independent_kernel=258 repeat_detach=3 expected=3')
    lines += ['MINIMUM_TERMINATED_OBJECT old_tid=101 actual_object_tid=101 actual_current_tid=104 kernel=0 os_tid_reuse_observed=0 would_be_self=0 optional_reuse_only=1',
        'MINIMUM_HELD_HELPER_READY before_target=1 native_only=1',
        'MINIMUM_HELD_BEFORE id=15 record_id=15 ended=1 stored_tid=105 actual_tid=105 borrowed_tid=105 kernel=258 cpp_constructed=1 cpp_returned=0 private_size=480',
        'MINIMUM_HELD_TRYJOIN rc=16 expected=16 result_unchanged=1 result_return=0 kernel_after=258 consumed=0',
        'MINIMUM_HELD_AFTER actual_kernel=0 cpp_returned=1 helper_kernel=0 helper_exit=0 target_exit=4660 normal_join=0 normal_return=1 consumed_id_rejoined=0',
        'MINIMUM_CONTRACTS_COMPLETE failures=0 original_census_unchanged=1 lifecycle_adoption_not_claimed=1']
    good='\n'.join(lines)
    check_control_trace('static','contracts',good,codes)
    check_control_trace('dynamic','contracts',good.replace('MINIMUM_PROVIDER dll=0','MINIMUM_PROVIDER dll=1 exact_executable_directory=1'),codes)
    rejects=[]
    rejects += ['\n'.join(lines[:i]+lines[i+1:]) for i in range(len(lines))]
    rejects += [good+'\n'+line for line in lines]
    for old,new in [('record_id=15','record_id=16'),('stored_tid=105','stored_tid=106'),('kernel_after=258','kernel_after=0'),('consumed=0','consumed=1'),('private_size=480','private_size=496'),('result_unchanged=1','result_unchanged=0'),('normal_return=1','normal_return=0'),('native_exit=61','native_exit=0'),('expected=36','expected=35'),('cpp_returned=0','cpp_returned=1'),('os_tid_reuse_observed=0','os_tid_reuse_observed=1'),('join_output_unchanged=1','join_output_unchanged=0')]:
        rejects.append(good.replace(old,new))
    altered=lines.copy();a=next(i for i,s in enumerate(altered) if s.startswith('MINIMUM_HELD_HELPER_READY'));b=a+1;altered[a],altered[b]=altered[b],altered[a];rejects.append('\n'.join(altered))
    count=0
    for text in rejects:
        try:check_control_trace('static','contracts',text,codes)
        except (AssertionError,KeyError):count+=1
        else:raise AssertionError('Malformed/incomplete minimum runtime trace accepted')
    for tag,expected in (('reference',0),('static',2),('dynamic',2)):
        provider='MINIMUM_PROVIDER dll=1 exact_executable_directory=1' if tag=='dynamic' else 'MINIMUM_PROVIDER dll=0'
        trace=provider+f'\nMINIMUM_ATTACHMENT expected_delta={expected} cold=60 after_key_setup=66 before_first_self=66 after_first_self={66+expected} after_second_self={66+expected} delta={expected} repeated_delta=0 first=10 second=10 native_tid=100 observation_only=1 original_census_unchanged=1'
        check_control_trace(tag,'attachment',trace,codes)
        for old,new in [('repeated_delta=0','repeated_delta=1'),('second=10','second=11'),('original_census_unchanged=1','original_census_unchanged=0'),('delta='+str(expected)+' repeated','delta=7 repeated')]:
            try:check_control_trace(tag,'attachment',trace.replace(old,new),codes)
            except (AssertionError,KeyError):count+=1
            else:raise AssertionError('Invalid attachment observation accepted')
    print('PASS minimum runtime trace row/output/native/ABI/order/errno negatives='+str(count)+'; synthetic metadata only, zero native operations')


if __name__ == '__main__':
    if sys.argv[1] == 'selftest':
        selftest()
    elif sys.argv[1] == 'build':
        build(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    elif sys.argv[1] == 'verify':
        verify()
    elif sys.argv[1] == 'trace':
        data=json.loads((DIST/'bundle.json').read_text());tag=sys.argv[2];mode=sys.argv[3]
        check_control_trace(tag,mode,Path(sys.argv[4]).read_text(encoding='utf-8-sig'),data['variants'][tag]['errno'])
        print('Verified actual minimum controls tag='+tag+' mode='+mode+'; no canonical lifecycle adoption inferred')
    else:
        raise RuntimeError('Unknown minimum bundle operation')
