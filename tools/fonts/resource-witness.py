"""Pure bounded signed-image metadata. No file opens, tools or live reads.

Caller authenticates full admitted image first. Static metadata anchors decoding;
this is not runtime stack/unwind/PDB integrity or an owner observation.
"""
import hashlib
import json
import struct
import re

UNWIND_DIAGNOSTIC_CAP = 4096

def _unwind_mismatch_evidence(entry, raw, code, operations):
    """Lossless bounded failure suffix only; formatting failure never admits data."""
    try:
        if type(raw)is not bytes or not 4<=len(raw)<=256 or type(code)is not bytes or len(code)<10:
            return ''
        if type(operations)is not list or len(operations)>32:
            return ''
        if not all(type(row)is tuple and len(row)==4 and
                   all(type(v)is int and 0<=v<2**32 for v in row) for row in operations):
            return ''
        selected={key:entry[key] for key in ('begin','end','unwind_rva','entry_hex')}
        if not all(type(selected[k])is int and 0<=selected[k]<2**32 for k in ('begin','end','unwind_rva')):
            return ''
        if type(selected['entry_hex'])is not str or len(selected['entry_hex'])!=24:
            return ''
        if len(bytes.fromhex(selected['entry_hex']))!=12:
            return ''
        evidence={'schema':'FONT_RESOURCE_UNWIND_MISMATCH_V1','selected_entry':selected,
                  'header_hex':raw[:4].hex(),'raw_hex':raw.hex(),
                  'raw_sha256':hashlib.sha256(raw).hexdigest(),
                  'version':raw[0]&7,'flags':raw[0]>>3,'count':raw[2],
                  'prolog':raw[1],'frame':raw[3],'received_operations':operations,
                  'expected_operations':[(10,4,3,48),(10,2,3,32),(6,0,7,0)],
                  'prologue_hex':code[:10].hex()}
        encoded=json.dumps(evidence,ensure_ascii=True,separators=(',',':'))
        suffix='\nFONT_RESOURCE_UNWIND_MISMATCH '+encoded
        if len(suffix.encode('ascii'))>UNWIND_DIAGNOSTIC_CAP:
            return ''
        return suffix
    except Exception:
        return ''

def require(value, reason):
    if not value: raise ValueError('UNAVAILABLE: '+reason)

def _fixed_body_state(operations):
    # Save locations are relative to immutable F=entrySP-40, NOT virtual RSP.
    fixed={48:'saved_RBX',32:'saved_RDI',40:'caller_return'}
    virtual=0;restored={}
    for offset,op,reg,value in operations:
        if op==4:
            require(reg==3 and value==48 and 'RBX'not in restored,'fixed RBX save slot')
            restored['RBX']=fixed[value]
        elif op==2:
            require(value==32,'fixed allocation');virtual+=value
        elif op==0:
            require(reg==7 and virtual==32 and 'RDI'not in restored,'fixed RDI push slot')
            restored['RDI']=fixed[virtual];virtual+=8
        else:raise ValueError('UNAVAILABLE: unsupported body inverse')
    require(virtual==40 and restored=={'RBX':'saved_RBX','RDI':'saved_RDI'} and fixed[virtual]=='caller_return','settled body inverse')
    return {'fixed_base':'entrySP-40','entry_sp_offset':virtual,'RBX_slot':48,'RDI_slot':32,
            'save_base_is_moving_virtual_sp':False,'restored':restored}

def mapped(data, sections, rva, size):
    require(type(rva)is int and type(size)is int and 0<=rva<2**32 and 0<size<=2**32-rva,'RVA arithmetic')
    offsets=[s['raw']+rva-s['rva'] for s in sections
             if s['rva']<=rva and rva+size<=s['rva']+s['raw_size']]
    require(len(offsets)==1,'unique file-backed metadata')
    at=offsets[0];require(0<=at<=len(data) and size<=len(data)-at,'metadata file bounds')
    return data[at:at+size]

def runtime_function(data, sections, directory, start):
    rva,size=directory
    require(rva%4==0 and 12<=size<=512*1024 and size%12==0 and size//12<=32768,'exception directory bounds')
    raw=mapped(data,sections,rva,size);entries=[];last=0
    for offset in range(0,size,12):
        begin,end,unwind=struct.unpack_from('<III',raw,offset)
        require(begin<end and begin>=last and unwind%4==0,'sorted disjoint runtime functions')
        require(len([s for s in sections if s['flags']&0x20000000 and s['rva']<=begin and end<=s['rva']+s['raw_size']])==1,'executable function extent')
        entries.append((begin,end,unwind,offset//12));last=end
    found=[v for v in entries if v[0]<=start+0x1c<v[1]]
    require(len(found)==1 and found[0][0]==start and start+0x29<found[0][1]<=start+96,'independent entry/full function coverage')
    begin,end,unwind,index=found[0]
    call=mapped(data,sections,start+36,5)
    require(call[0]==0xe8,'direct held-handler callee call')
    target=start+41+int.from_bytes(call[1:],'little',signed=True)
    callees=[v for v in entries if v[0]==target]
    require(len(callees)==1 and 0<callees[0][1]-target<=4096,'unique bounded nolock callee metadata')
    return {'begin':begin,'end':end,'unwind_rva':unwind,'index':index,
            'nolock_begin':target,'nolock_end':callees[0][1],
            'entry_hex':raw[index*12:index*12+12].hex(),'directory_rva':rva,
            'directory_bytes':size,'directory_sha256':hashlib.sha256(raw).hexdigest()}

def unwind_prologue(data, sections, entry, code):
    # Exact observed saveRBX/pushRDI/alloc32 prologue. Reject unseen variants,
    # chained records and unverified unwind modes instead of guessing.
    at=entry['unwind_rva'];head=mapped(data,sections,at,4)
    version=head[0]&7;flags=head[0]>>3;prolog=head[1];count=head[2];frame=head[3]
    require(version==1 and flags in (0,1,2,3) and count<=32 and frame==0,'supported unwind header')
    size=4+((count+1)//2)*4+(4 if flags else 0)
    require(size<=256,'unwind cap');raw=mapped(data,sections,at,size)
    require(code[:10]==bytes.fromhex('48895c2408574883ec20') and prolog==10,'full observed prologue')
    operations=[];i=0;last=256
    while i<count:
        off=raw[4+i*2];op=raw[5+i*2]&15;info=raw[5+i*2]>>4
        require(0<off<=prolog and off<=last,'unwind offsets');last=off
        if op==4: # UWOP_SAVE_NONVOL, one extra ushort, scaled by8
            require(i+1<count,'save-nonvolatile slot');saved=struct.unpack_from('<H',raw,6+i*2)[0]*8
            operations.append((off,op,info,saved));i+=2
        elif op==0:operations.append((off,op,info,0));i+=1
        elif op==2:operations.append((off,op,info,info*8+8));i+=1
        else:raise ValueError('UNAVAILABLE: unseen unwind opcode')
    reason='prologue/unwind register and stack coherence'
    if operations!=[(10,4,3,48),(10,2,3,32),(6,0,7,0)]:
        reason+=_unwind_mismatch_evidence(entry,raw,code,operations)
    require(operations==[(10,4,3,48),(10,2,3,32),(6,0,7,0)],reason)
    body=_fixed_body_state(operations)
    handler=None
    if flags:
        handler=struct.unpack_from('<I',raw,size-4)[0]
        require(len([s for s in sections if s['flags']&0x20000000 and s['rva']<=handler<s['rva']+s['raw_size']])==1,'opaque handler RVA')
    return {'rva':at,'bytes':size,'hex':raw.hex(),'sha256':hashlib.sha256(raw).hexdigest(),
            'version':version,'flags':flags,'prolog_bytes':prolog,'operations':operations,
            'opaque_handler_rva':handler,'scope':'POST_PROLOGUE_COMMON_BODY',
            'partial_prologue_qualified':False,'runtime_unwind_integrity':False,
            'fixed_body_state':body}

def virtual_object_bounds(sections, candidate, image_size):
    require(type(candidate)is int and candidate%8==0 and 0<=candidate<=image_size-0x28,'object RVA/extent')
    found=[s for s in sections if s['flags']&0x80000000 and not s['flags']&0x20000000 and
           s['rva']<=candidate and candidate+0x28<=s['rva']+s['extent']]
    require(len(found)==1,'unique writable nonexec virtual object section')
    return {'rva':found[0]['rva'],'extent':found[0]['extent'],'flags':found[0]['flags'],
            'object_bytes':0x28,'file_backed_object_required':False}

def controlflow_basis(cache):
    code=bytes.fromhex(cache['cache_hex']);start=cache['start_rva'];end=cache['function']['end']-start
    unwind=cache['unwind']
    require(type(unwind['scope'])is str and unwind['scope']=='POST_PROLOGUE_COMMON_BODY' and
            type(unwind['partial_prologue_qualified'])is bool and unwind['partial_prologue_qualified']is False and
            type(unwind['runtime_unwind_integrity'])is bool and unwind['runtime_unwind_integrity']is False,
            'typed bounded body scope')
    require(unwind['prolog_bytes']==10 and 10<29<58 and 10<41<58,'conservative body control PCs')
    # Decode EVERY primary-function instruction using this exact known template.
    # Only two signed RIP operands and the already-observed directcall vary.
    expected=[(0,'48895c2408'),(5,'57'),(6,'4883ec20'),(10,'488bda'),(13,'488bf9'),
              (29,'90'),(30,'488bd3'),(33,'488bcf'),(41,'8bd8'),(56,'8bc3'),
              (58,'488b5c2430'),(63,'4883c420'),(67,'5f'),(68,'c3')]
    require(end==69,'entire primary function template size')
    for offset,hexvalue in expected:
        value=bytes.fromhex(hexvalue);require(code[offset:offset+len(value)]==value,'full function instruction template')
    require(code[16:19]==bytes.fromhex('488d0d') and code[23:25]==bytes.fromhex('ff15') and
            code[36]==0xe8 and code[43:46]==bytes.fromhex('488d0d') and code[50:52]==bytes.fromhex('ff15'),'argument/call/release instruction forms')
    candidate=start+23+int.from_bytes(code[19:23],'little',signed=True)
    release=start+50+int.from_bytes(code[46:50],'little',signed=True)
    require(candidate==release,'same acquired/released object expression')
    bounds=virtual_object_bounds(cache['section_facts'],candidate,cache['size_of_image'])
    return {'function_begin':start,'function_end':start+69,'lea_rva':start+16,
            'enter_call_rva':start+23,'writer_return_rva':start+29,
            'nolock_call_rva':start+36,'handler_return_rva':start+41,
            'nolock_target_rva':start+41+int.from_bytes(code[37:41],'little',signed=True),
            'candidate_rva':candidate,'virtual_object':bounds,
            'instruction_ends':[5,6,10,13,16,23,29,30,33,36,41,43,50,56,58,63,67,68,69],
            'scope':'POST_PROLOGUE_COMMON_BODY','partial_prologue_qualified':False,
            'runtime_unwind_integrity':False,'body_after_rva':start+10,'body_before_rva':start+58,
            'owner_observed':False,'runtime_unwind_integrity_attested':False}

# DISK metadata only. These routines accept bytes from an already authenticated
# retained lease; they never open a path, query MUI, or inspect loaded memory.
DISK_ROLE = 'SIGNED_SYSTEM32_NTDLL_DISK_METADATA'
DISK_MAX = 128*1024*1024
DISK_PROFILES = {
 'bcfe8a70a371a7d2fbcb429ab0a9e170aeebe4d73ae08d03a2da8fedec2efc78':'10.0.20348.5622',
 '160ef9b82e2895ef48fa031cb1cf683a8b123febf7cf7d5590cd3104864424b3':'10.0.26100.33648',
}

def disk_pe(data):
    require(type(data)is bytes and 64<=len(data)<=DISK_MAX,'disk file cap')
    def take(at,n):
        require(type(at)is int and type(n)is int and 0<=at<=len(data) and 0<=n<=len(data)-at,'disk file span')
        return data[at:at+n]
    def unpack(fmt,at):return struct.unpack(fmt,take(at,struct.calcsize(fmt)))
    require(take(0,2)==b'MZ','disk DOS')
    pe=unpack('<I',60)[0];require(pe>=64 and take(pe,4)==b'PE\0\0','disk PE')
    machine,count,stamp=unpack('<HHI',pe+4)
    require(machine==0x8664 and 1<=count<=96,'disk AMD64 sections')
    opt_size=unpack('<H',pe+20)[0];opt=pe+24;take(opt,opt_size)
    require(opt_size>=112 and unpack('<H',opt)[0]==0x20b,'disk PE32+')
    image,headers,checksum=unpack('<III',opt+56)
    dirs=unpack('<I',opt+108)[0]
    require(3<=dirs<=16 and opt_size>=112+8*dirs,'disk directory table')
    table=opt+opt_size;take(table,count*40)
    require(table+count*40<=headers<=len(data) and headers<=image,'disk headers/image')
    sections=[];vr=[(0,headers)];rr=[(0,headers)]
    for i in range(count):
        at=table+i*40;vs,rva,size,raw=unpack('<IIII',at+8);extent=max(vs,size)
        require(0<extent<=2**32-rva and 0<rva and rva+extent<=image,'disk virtual span')
        require(all(rva+extent<=lo or rva>=hi for lo,hi in vr),'disk virtual overlap')
        vr.append((rva,rva+extent))
        if size:
            take(raw,size);require(all(raw+size<=lo or raw>=hi for lo,hi in rr),'disk raw overlap');rr.append((raw,raw+size))
        sections.append({'rva':rva,'raw':raw,'raw_size':size})
    def directory(index):return unpack('<II',opt+112+index*8)if index<dirs else (0,0)
    resource=directory(2);require(resource[0]>0 and resource[1]>=16,'disk resource directory')
    raw=mapped(data,sections,*resource)
    # Debug type16 IMAGE_DEBUG_TYPE_REPRO means the COFF field is NOT a clock.
    repro=[];debug=directory(6)
    if debug!=(0,0):
        require(debug[0]>0 and 0<debug[1]<=64*28 and debug[1]%28==0,'disk debug bounds')
        records=mapped(data,sections,*debug)
        for at in range(0,len(records),28):
            _,_,_,_,kind,n,rva,offset=struct.unpack_from('<IIHHIIII',records,at)
            if kind==16:
                require(n<=65536 and (n==0 or rva>0),'disk repro cap')
                payload=mapped(data,sections,rva,n)if n else b''
                require(take(offset,n)==payload,'disk repro file/RVA equality')
                repro.append({'bytes':n,'sha256':hashlib.sha256(payload).hexdigest()})
    return sections,resource,raw,{'machine':machine,'section_count':count,'size_of_image':image,
        'size_of_headers':headers,'checksum':checksum,'coff_timestamp_raw':stamp,
        'coff_timestamp_is_wall_clock_attested':False,'repro_records':repro}

def version_payload(payload):
    require(type(payload)is bytes and 0<len(payload)<65536,'version WORD/file bound')
    nodes=0
    def align(n):return (n+3)&~3
    def zeros(lo,hi,end):
        require(lo<=hi<=end and not any(payload[lo:hi]),'version alignment padding')
    def block(at,parent):
        nonlocal nodes
        nodes+=1;require(nodes<=256 and at%4==0 and at+6<=parent,'version node bounds')
        length,value_length,kind=struct.unpack_from('<HHH',payload,at)
        end=at+length;require(length>=8 and end<=parent and kind in (0,1),'version block length/type')
        cursor=at+6;units=[]
        while True:
            require(cursor+2<=end and len(units)<=128,'version key bounds')
            unit=struct.unpack_from('<H',payload,cursor)[0];cursor+=2
            if unit==0:break
            units.append(unit)
        try:key=struct.pack('<'+'H'*len(units),*units).decode('utf-16-le','strict')
        except UnicodeError:raise ValueError('UNAVAILABLE: version key UTF16')
        value_at=align(cursor);zeros(cursor,value_at,end)
        count=value_length*(2 if kind==1 else 1)
        require(count<=end-value_at,'version value bounds/units')
        value=payload[value_at:value_at+count];cursor=value_at+count;children=[]
        while cursor<end:
            next_at=align(cursor);zeros(cursor,min(next_at,end),end)
            if next_at>=end:break
            child=block(next_at,end);children.append(child);cursor=child['end']
        return {'at':at,'end':end,'key':key,'type':kind,'value_length':value_length,'value':value,'children':children}
    root=block(0,len(payload))
    require(root['end']<=len(payload) and len(payload)-root['end']<=8 and root['key']=='VS_VERSION_INFO' and
            root['type']==0 and root['value_length']==52,'version root/fixed52')
    zeros(root['end'],len(payload),len(payload))
    fixed=list(struct.unpack('<13I',root['value']))
    require(fixed[0]==0xfeef04bd and fixed[1]==0x10000,'fixed signature/structure version')
    def components(ms,ls):return [ms>>16,ms&65535,ls>>16,ls&65535]
    tables=[];translations=[];seen=set()
    for group in root['children']:
        require(group['key']in ('StringFileInfo','VarFileInfo') and group['key']not in seen and
                group['type']==1 and group['value_length']==0,'version unique supported child')
        seen.add(group['key'])
        if group['key']=='StringFileInfo':
            require(len(group['children'])<=64,'string table count');keys=set()
            for table in group['children']:
                require(re.fullmatch('[0-9A-Fa-f]{8}',table['key'])is not None and
                        table['key'].lower()not in keys and table['type']==1 and table['value_length']==0,'unique language/codepage table')
                keys.add(table['key'].lower());strings={}
                require(len(table['children'])<=64,'string count')
                for item in table['children']:
                    require(item['key'] and item['key']not in strings and item['type']==1 and
                            not item['children'] and 1<=item['value_length']<=4096,'version unique string')
                    try:text=item['value'].decode('utf-16-le','strict')
                    except UnicodeError:raise ValueError('UNAVAILABLE: version value UTF16')
                    require(text.endswith('\0') and '\0'not in text[:-1],'version string terminator')
                    strings[item['key']]=text[:-1]
                tables.append({'language':int(table['key'][:4],16),'codepage':int(table['key'][4:],16),'strings':strings})
        else:
            require(len(group['children'])==1,'translation child count')
            item=group['children'][0]
            require(item['key']=='Translation' and item['type']==0 and not item['children'] and
                    0<item['value_length']<=256 and item['value_length']%4==0,'translation binary length')
            for at in range(0,len(item['value']),4):
                pair=list(struct.unpack_from('<HH',item['value'],at))
                require(pair not in translations,'duplicate translation');translations.append(pair)
    fields=('signature','structure_version','file_version_ms','file_version_ls','product_version_ms','product_version_ls',
            'file_flags_mask','file_flags','file_os','file_type','file_subtype','file_date_ms','file_date_ls')
    return {'fixed_dwords':fixed,'fixed_fields':dict(zip(fields,fixed)),'file_version_components':components(fixed[2],fixed[3]),
            'product_version_components':components(fixed[4],fixed[5]),'string_tables':tables,
            'translations':translations,'raw_bytes':len(payload),'raw_sha256':hashlib.sha256(payload).hexdigest()}

def _resource_directory_rejection(raw,rva,size,at,level,keys,provenance):
    try:
        if type(provenance)is not dict or set(provenance)!={'profile','context','imports_pid','input_record_sha256'}:return ''
        if type(raw)is not bytes or not (0<=at<=len(raw)-16) or type(keys)is not list or len(keys)>2:return ''
        header=raw[at:at+16];characteristics,timestamp,major,minor,named,ids=struct.unpack('<IIHHHH',header)
        record={'schema':'FONT_RESOURCE_RESOURCE_DIRECTORY_REJECTION_V1','provenance':provenance,
                'resource_rva':rva,'resource_bytes':size,'resource_sha256':hashlib.sha256(raw).hexdigest(),
                'level':level,'relative_offset':at,'key_path':keys,'header_hex':header.hex(),
                'header_sha256':hashlib.sha256(header).hexdigest(),
                'characteristics':characteristics,'timestamp_raw':timestamp,'major':major,'minor':minor,
                'named':named,'ids':ids,'count':named+ids,'named_zero':named==0,
                'named_allowed':named==0 or level==0,'count_in_range':0<named+ids<=64}
        suffix='\nFONT_RESOURCE_RESOURCE_DIRECTORY_REJECTION '+json.dumps(record,ensure_ascii=True,separators=(',',':'))
        if len(suffix.encode('ascii'))>8192:return ''
        return suffix
    except Exception:
        return ''

def disk_version_metadata(data,provenance=None):
    sections,(rva,size),raw,headers=disk_pe(data)
    visited=0;claimed=[];leaves=[];names=set();ignored_roots=[]
    def claim(at,n):
        require(0<=at<=len(raw) and 0<n<=len(raw)-at,'resource relative span')
        require(all(at+n<=lo or at>=hi for lo,hi in claimed),'resource cycle/alias/overlap')
        claimed.append((at,at+n))
    def directory(at,level,keys):
        nonlocal visited
        require(level<=2 and at%4==0,'resource depth/alignment');claim(at,16)
        named,ids=struct.unpack_from('<HH',raw,at+12);count=named+ids
        reason='resource numeric identifier/count'
        if not ((named==0 or level==0) and 0<count<=64):
            try:reason+=_resource_directory_rejection(raw,rva,size,at,level,keys,provenance)
            except Exception:pass
        require((named==0 or level==0) and 0<count<=64,reason)
        claim(at+16,count*8);local=set();visited+=1+count
        require(visited<=256,'resource aggregate visited')
        rows=[];previous_name=None;previous_id=-1
        for i in range(count):
            name,target=struct.unpack_from('<II',raw,at+16+i*8)
            if i<named:
                require(level==0 and bool(name&0x80000000),'resource named type flag')
                string_at=name&0x7fffffff
                require(string_at%2==0,'resource name alignment');claim(string_at,2)
                length=struct.unpack_from('<H',raw,string_at)[0]
                require(0<length<=256,'resource name length');claim(string_at+2,length*2)
                value=raw[string_at+2:string_at+2+length*2]
                decoded=value.decode('utf-16-le');require('\0' not in decoded,'resource name embedded NUL')
                units=struct.unpack('<'+'H'*length,value)
                require(previous_name is None or previous_name<units,'resource duplicate/unsorted name')
                previous_name=units
                target_at=target&0x7fffffff
                require(bool(target&0x80000000) and target_at%4==0 and 0<target_at<=len(raw)-16,'resource named type target')
                require(all(target_at+16<=lo or target_at>=hi for lo,hi in ignored_roots),'resource named type target alias')
                ignored_roots.append((target_at,target_at+16))
                continue
            require(name<=65535 and name not in local,'resource duplicate/named key');local.add(name)
            require(name>previous_id,'resource unsorted numeric key');previous_id=name
            rows.append((name,target))
        for name,target in rows:
            # Other resource types are not interpreted or recursively scanned.
            if level==0 and name!=16:continue
            if level<2:
                require(target&0x80000000,'resource exact Type/Name/Language depth')
                directory(target&0x7fffffff,level+1,keys+[name])
            else:
                require(not target&0x80000000 and target%4==0,'resource leaf depth/alignment')
                claim(target,16);drva,n,cp,reserved=struct.unpack_from('<IIII',raw,target)
                require(reserved==0 and drva%4==0 and 0<n<65536 and len(leaves)<8,'resource leaf WORD/count/alignment')
                require(sum(v['bytes']for v in leaves)+n<=512*1024,'resource aggregate payload cap')
                key=tuple(keys+[name]);require(key not in names,'resource duplicate path');names.add(key)
                payload=mapped(data,sections,drva,n)
                require(all((s['raw']+drva-s['rva'])%4==0 for s in sections if s['rva']<=drva and drva+n<=s['rva']+s['raw_size']),'resource file payload alignment')
                # Payloads may live outside the directory extent, but never
                # overlap structures or another payload in the same image.
                leaves.append({'path':list(key),'rva':drva,'bytes':n,'codepage':cp,'payload':payload})
    directory(0,0,[]);require(leaves,'RT_VERSION absent')
    require(all(hi<=a or lo>=b for lo,hi in ignored_roots for a,b in claimed),'resource named target/structure overlap')
    spans=[];languages=set();version_names=set()
    for leaf in leaves:
        language=leaf['path'][2];version_names.add(leaf['path'][1])
        require(language not in languages and len(version_names)==1,'ambiguous version name/language');languages.add(language)
        lo=leaf['rva'];hi=lo+leaf['bytes']
        require(all(hi<=a or lo>=b for a,b in spans),'resource shared payload alias')
        require(all(hi<=rva+a or lo>=rva+b for a,b in claimed),'resource payload/structure overlap')
        require(all(hi<=rva+a or lo>=rva+b for a,b in ignored_roots),'resource payload/named target overlap')
        spans.append((lo,hi))
        leaf['version']=version_payload(leaf.pop('payload'))
    require(all(v['version']['fixed_dwords']==leaves[0]['version']['fixed_dwords']for v in leaves),'conflicting fixed records')
    return {'headers':headers,'resource_rva':rva,'resource_bytes':size,
            'resource_sha256':hashlib.sha256(raw).hexdigest(),'records':leaves,
            'loaded_memory_identity_attested':False,'identity_predicate_repaired':False}

def admit_disk_metadata(data,entry,context,pid):
    require(type(entry)is dict and set(entry)=={'schema','role','name','bytes','sha256','version','path','signature_status','signer','signer_thumbprint','lease','context','imports_pid'},'disk frame schema')
    require(type(entry['schema'])is int and entry['schema']==1 and entry['role']==DISK_ROLE and entry['name']=='ntdll.dll','disk role/schema')
    require(type(context)is dict and set(context)=={'source','run','attempt'} and type(context['source'])is str and
            re.fullmatch('[0-9a-f]{40}',context['source'])and all(type(context[k])is str and re.fullmatch('[1-9][0-9]{0,19}',context[k])for k in ('run','attempt')),'disk current context')
    require(entry['context']==context and type(pid)is int and 0<pid<=0xffffffff and type(entry['imports_pid'])is int and entry['imports_pid']==pid,'disk same imports PID/context')
    require(type(data)is bytes and type(entry['bytes'])is int and 64<=entry['bytes']<=DISK_MAX and len(data)==entry['bytes'],'disk full file length')
    digest=hashlib.sha256(data).hexdigest()
    require(entry['sha256']==digest and digest in DISK_PROFILES and entry['version']==DISK_PROFILES[digest],'disk pinned signed input hash/version')
    require(type(entry['path'])is str and re.fullmatch(r'[A-Za-z]:\\(?:[^\\/:*?"<>|\x00-\x1f]+\\)*System32\\ntdll\.dll',entry['path'],re.I)and
            all(v not in ('.','..')and not v.endswith((' ','.'))for v in entry['path'].split('\\')[1:]),'disk native System32 path')
    require(entry['signature_status']=='Valid' and type(entry['signer'])is str and
            re.search(r'(?:^|,\s*)O=Microsoft Corporation(?:,|$)',entry['signer'])and type(entry['signer_thumbprint'])is str and
            re.fullmatch('[A-Fa-f0-9]{40,128}',entry['signer_thumbprint'])and entry['lease']=='RETAINED_READ_SHARE_READ','disk signature/retained lease')
    input_hash=hashlib.sha256(json.dumps(entry,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    provenance={'profile':{k:entry[k]for k in ('role','name','version','bytes','sha256')},'context':context,'imports_pid':pid,'input_record_sha256':input_hash}
    return {'record':'NTDLL_DISK_METADATA','role':DISK_ROLE,'profile':entry,'context':context,'imports_pid':pid,
            'input_record_sha256':input_hash,'metadata':disk_version_metadata(data,provenance),'qualification':False}
