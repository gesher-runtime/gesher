"""Pure bounded signed-image metadata. No file opens, tools or live reads.

Caller authenticates full admitted image first. Static metadata anchors decoding;
this is not runtime stack/unwind/PDB integrity or an owner observation.
"""
import hashlib
import json
import struct

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
