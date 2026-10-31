"""Pure sealed producer/current-API receipt checks. No fetch or native actions.

All arguments are data. Their authenticity must be established by a separately
pinned immutable workflow, never by a success string/manifest supplied by the
producer itself. No source authority is granted by this module.
"""
import re
from pathlib import PurePosixPath
from source_contract import (REPO,RID,CALLER,POLICY,WORKFLOW,BRANCH,SCOPE,
    require,keys,hexid,positive,canonical,digest,validate_context,validate_freeze)
OUTPUT_ROLES=tuple(sorted((
 'nativehost-alias','nativehost-lifetime','nativehost-failure-lifetime','nativehost-prepare',
 'x86_64-lifetime','arm64-v8a-lifetime','runtime-current','translator-current',
 'x86_64-libraries','arm64-v8a-libraries','compiler-current',
 'mman-production-object','helper-production-object','mman-failure-object',
 'helper-qualification-object','noexcept-caller-object')))
OBJECTS={
 'mman-production-object':('compat/posix/src/mman.cc',False,False),
 'mman-failure-object':('compat/posix/src/mman.cc',False,True),
 'helper-production-object':('compat/posix/src/mman_alias_prepare.cc',True,False),
 'helper-qualification-object':('compat/posix/src/mman_alias_prepare.cc',True,False),
 'noexcept-caller-object':('tests/file_mremap/alias_prepare_noexcept_caller.cc',False,False)}
def safe_member(name):
    require(type(name)is str and 0<len(name)<=240 and '\\'not in name and ':'not in name and re.fullmatch('[A-Za-z0-9._/-]+',name),'artifact member grammar')
    path=PurePosixPath(name)
    require(not path.is_absolute()and all(p not in ('','.','..')for p in name.split('/')),'artifact member traversal')
    return name
def check_run(run,context,policy_commit):
    validate_context(context);hexid(policy_commit,40)
    require(type(run)is dict,'run object')
    require(str(run['id'])==context['run']and str(run['run_attempt'])==context['attempt'],'current attempt')
    require(run['head_sha']==context['source']and run['head_branch']==BRANCH,'run source/branch')
    require(run['path']==CALLER and run['name']==WORKFLOW and run['event']in ('push','workflow_dispatch'),'focused workflow')
    for field in ('repository','head_repository'):
        require(run[field]['full_name']==REPO and str(run[field]['id'])==RID,'fork/repository identity')
    require(run['actor']['login']==run['triggering_actor']['login']=='palmoni5','actor identity')
    refs=run['referenced_workflows']
    require(len(refs)==1 and refs[0]['path']==REPO+'/'+POLICY+'@'+policy_commit and refs[0]['sha']==policy_commit,'actual immutable policy')
    return run
def current_job(jobs,context,name):
    keys(jobs,('total_count','jobs'))
    require(type(jobs['jobs'])is list and type(jobs['total_count'])is int and jobs['total_count']==len(jobs['jobs'])<=100,'incomplete/paginated job snapshot')
    require(len({j['id']for j in jobs['jobs']})==len(jobs['jobs']),'duplicate job IDs')
    require(len({j['name']for j in jobs['jobs']})==len(jobs['jobs']),'duplicate role names')
    require({j['name']for j in jobs['jobs']}<={'source_authority / verify','produce','linux-reference','windows-2022','windows-2025'},'unexpected workflow role')
    for row in jobs['jobs']:
        positive(row['id'])
        require(str(row['run_id'])==context['run']and str(row['run_attempt'])==context['attempt']and row['head_sha']==context['source'],'foreign/stale workflow job')
    guard=[j for j in jobs['jobs']if j['name']=='source_authority / verify']
    require(len(guard)==1 and guard[0]['status']=='completed'and guard[0]['conclusion']=='success','source authority not successful')
    selected=[j for j in jobs['jobs']if j['name']==name]
    require(len(selected)==1,'exact producer role missing/ambiguous')
    job=selected[0];positive(job['id'])
    require(str(job['run_id'])==context['run']and str(job['run_attempt'])==context['attempt']and job['head_sha']==context['source'],'job source/attempt')
    require(job['status']=='completed'and job['conclusion']=='success','producer not terminal success')
    require(job['started_at']and job['completed_at'],'job lifetime missing')
    return job

def compile_stamp(stamp,role,inputs,compiler):
    keys(stamp,('source','source_sha256','object_sha256','driver_sha256','arguments','stl_abi','allocator_abi'))
    source,exceptions,backend=OBJECTS[role]
    require(stamp['source']==source and stamp['source_sha256']==inputs[source]['sha256'],'object source binding')
    hexid(stamp['object_sha256'],64)
    require(stamp['driver_sha256']==compiler['driver_sha256'],'object compiled with different driver')
    require(stamp['stl_abi']==compiler['stl_abi']and stamp['allocator_abi']==compiler['allocator_abi'],'allocator/STL ABI mismatch')
    argv=stamp['arguments'];require(type(argv)is list and 2<=len(argv)<=1024 and all(type(v)is str and len(v)<=4096 for v in argv),'compile argv')
    switches=[v for v in argv if v in ('-fexceptions','-fno-exceptions')]
    require(switches and switches[-1]==('-fexceptions'if exceptions else '-fno-exceptions'),'effective exception mode')
    defines=[v for v in argv if v.startswith('-DGESHER_FILE_MREMAP_FAILURE_TEST')]
    require(defines==(['-DGESHER_FILE_MREMAP_FAILURE_TEST=1']if backend else []),'backend leakage/missing test object')
    require(not any(v.startswith('@')or v.startswith('-Wl,')for v in argv),'unexpanded/noncompile command')
    require('-c'in argv and '-o'in argv,'compile command not a source-object command')
    return stamp

def helper_qualification(value):
    keys(value,('exit','stdout_sha256','stderr_sha256','source_role','allocation_rows','cleanup_completed'))
    require(value['source_role']=='nativehost-prepare'and type(value['exit'])is int and value['exit']==0 and value['cleanup_completed']is True,'helper qualification failed')
    for field in ('stdout_sha256','stderr_sha256'):hexid(value[field],64)
    rows=value['allocation_rows'];require(type(rows)is list and rows,'actual allocation rows missing')
    require({r['family']for r in rows}=={'alias','surgery'},'helper family missing')
    for family in ('alias','surgery'):
        series=[r for r in rows if r['family']==family]
        require([r['budget']for r in series]==list(range(len(series))),'budget coverage/order')
        require(len(series)>=2 and series[-1]['status']=='OK','no actual successful preparation')
        for row in series:
            keys(row,('family','budget','allocation_calls','injected','status','outstanding','unchanged_owner_refs'))
            require(type(row['allocation_calls'])is int and row['allocation_calls']>0,'direct stub/no allocation exercised')
            require(row['outstanding']==0 and row['unchanged_owner_refs']is True,'preparation ownership violation')
            require(row['status']in ('OK','NO_MEMORY'),'unexpected caught status')
            require(type(row['injected'])is int and row['injected']==(1 if row['status']=='NO_MEMORY'else 0),'actual allocation fault not reached')
    # This direct-helper budget test measures allocation/ownership only. The
    # producer/caller must separately establish real pre-prepare OS call counts
    # at the promoted production entry points; no constant zero is invented.
    return value

def validate_producer(value,freeze,context,policy_commit,run,jobs,artifact):
    """Validate a COMPLETE current full-A producer, not a host-only shortcut."""
    validate_freeze(freeze);validate_context(context);check_run(run,context,policy_commit)
    job=current_job(jobs,context,'produce')
    keys(value,('schema','scope','context','role','source_manifest_sha256','producer_id','artifact_id','artifact_digest','compiler','outputs','object_stamps','helper_qualification'))
    require(type(value['schema'])is int and value['schema']==1 and value['scope']==SCOPE,'producer scope')
    require(value['role']=='file-mremap-producer'and value['context']==context,'producer role/attempt')
    require(value['source_manifest_sha256']==digest(canonical(freeze))and freeze['source']==context['source'],'producer source freeze')
    require(value['producer_id']==job['id'],'wrong role supplier')
    positive(value['artifact_id']);require(re.fullmatch('sha256:[0-9a-f]{64}',value['artifact_digest']),'artifact digest')
    require(artifact['id']==value['artifact_id']and artifact['name']=='file-mremap-produced-'+context['source']+'-'+context['run']+'-'+context['attempt'],'artifact identity/name')
    require(artifact['digest']==value['artifact_digest']and artifact['expired']is False and artifact['size_in_bytes']>0,'artifact bytes unavailable/expired')
    require(str(artifact['workflow_run']['id'])==context['run']and artifact['workflow_run']['head_sha']==context['source'] and str(artifact['workflow_run']['repository_id'])==str(artifact['workflow_run']['head_repository_id'])==RID,'artifact run/source/repository')
    compiler=value['compiler'];keys(compiler,('driver_sha256','recipe_fingerprint','receipt_sha256','supplier_role','stl_abi','allocator_abi'))
    for key in ('driver_sha256','recipe_fingerprint','receipt_sha256'):hexid(compiler[key],64)
    require(compiler['supplier_role']=='production-current-selected'and type(compiler['stl_abi'])is str and 0<len(compiler['stl_abi'])<=200 and type(compiler['allocator_abi'])is str and 0<len(compiler['allocator_abi'])<=200,'compiler role/ABI')
    outputs=value['outputs'];require(type(outputs)is list and [v['role']for v in outputs]==list(OUTPUT_ROLES),'output roles incomplete/duplicate/order')
    paths=set()
    for row in outputs:
        keys(row,('role','member','sha256','size','source','producer_id'))
        safe_member(row['member']);require(row['member']not in paths,'duplicate artifact member');paths.add(row['member'])
        hexid(row['sha256'],64);positive(row['size'])
        require(row['source']==context['source']and row['producer_id']==job['id'],'foreign/stale output supplier')
    require(next(v['sha256']for v in outputs if v['role']=='compiler-current')==compiler['receipt_sha256'],'unbound supplier receipt bytes')
    inputs={v['path']:v for v in freeze['inputs']}
    require(set(value['object_stamps'])==set(OBJECTS),'missing object qualification')
    for role,stamp in value['object_stamps'].items():
        compile_stamp(stamp,role,inputs,compiler)
        require(stamp['object_sha256']==next(v['sha256']for v in outputs if v['role']==role),'unbound object bytes')
    helper_qualification(value['helper_qualification'])
    return value
