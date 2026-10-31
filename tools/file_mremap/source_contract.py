"""Pure SOURCE contract, not an execution or remote producer authority.

An immutable reusable gate must construct the expected anchors from its own
reviewed Git objects and current authenticated GitHub metadata. Loading the
same JSON twice or accepting a plan's claimed policy is NOT that gate. No
network, artifact fetch, compiler, PE parsing or native operation lives here.
"""
import hashlib,json,re,subprocess
from pathlib import Path
BASE='2e41f6d21b21639d586986b83e2a05df9bc7b688'
REPO='gesher-runtime/gesher';RID='1426153186'
BRANCH='wip/file-mremap-shared-alias'
CALLER='.github/workflows/file-mremap-component.yml'
POLICY='.github/workflows/file-mremap-trusted-source.yml'
WORKFLOW='file-mremap-component'
SCOPE='PARTIAL_A_ALIAS_EQUAL_ONLY'
INPUTS=tuple(sorted((
 'CMakeLists.txt','compat/posix/posix.cmake','compat/posix/src/mman.cc',
 'compat/posix/src/mman_alias_prepare.cc','compat/posix/src/mman_registry_internal.h',
 'docs/handoff/tracks/file-mremap-shared-alias.md',
 'tests/file_mremap/CMakeLists.txt','tests/file_mremap/shared_alias.c',
 'tests/file_mremap/alias_failure_adapter.cc','tests/file_mremap/alias_failure_adapter.h',
 'tests/file_mremap/alias_failure_test.c','tests/file_mremap/alias_prepare_test.cc',
 'tests/file_mremap/alias_prepare_noexcept_caller.cc','tests/file_mremap/lifetime_cases.c',
 'tools/file_mremap/oracle.py','tools/file_mremap/test_oracle.py',
 'tools/file_mremap/reference.py','tools/file_mremap/windows_controller.py',
 'tools/file_mremap/source_contract.py','tools/file_mremap/producer_receipt.py',
 'tools/file_mremap/test_source_contract.py')))
ROOT_BLOCK=(b'\n# BEGIN FILE_MREMAP_SOURCE_QUALIFICATION\n'
 b'# Explicit CI qualification targets; no fixture is part of the default product.\n'
 b'add_subdirectory(tests/file_mremap EXCLUDE_FROM_ALL)\n'
 b'# END FILE_MREMAP_SOURCE_QUALIFICATION\n')
class ContractError(ValueError):pass
def require(ok,message):
    if not ok:raise ContractError(message)
def keys(value,expected):
    require(type(value)is dict and set(value)==set(expected),'field set')
def hexid(value,length):
    require(type(value)is str and re.fullmatch('[0-9a-f]{'+str(length)+'}',value),'invalid hash')
def positive(value):
    require(type(value)is int and value>0,'positive integer required')
def digest(raw):return hashlib.sha256(raw).hexdigest()
def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=True).encode()+b'\n'
def decode(raw):
    require(type(raw)is bytes and len(raw)<=16*1024*1024,'JSON byte bound')
    def unique(pairs):
        result={}
        for name,value in pairs:
            require(name not in result,'duplicate JSON key');result[name]=value
        return result
    return json.loads(raw,object_pairs_hook=unique)
def git(root,*args):
    return subprocess.check_output(['git','-C',str(root),*args])
def inventory(root,commit):
    hexid(commit,40);result={}
    for row in filter(None,git(root,'ls-tree','-r','-z',commit).split(b'\0')):
        metadata,path=row.split(b'\t',1);mode,kind,blob=metadata.decode().split()
        name=path.decode('utf-8');require(kind=='blob'and mode in ('100644','100755'),'linked/nonregular source')
        require(name not in result,'duplicate inventory path');result[name]=dict(mode=mode,blob=blob)
    return result
def freeze(root,source):
    """Read exact immutable Git objects, never assume the live checkout is C."""
    hexid(source,40)
    require(subprocess.run(['git','-C',str(root),'merge-base','--is-ancestor',BASE,source],capture_output=True).returncode==0,'base not ancestor')
    old=inventory(root,BASE);current=inventory(root,source)
    changed={p for p in set(old)|set(current)if old.get(p)!=current.get(p)}
    require(changed<=set(INPUTS),'source envelope expanded without review')
    require(set(INPUTS)<=set(current),'source inputs missing')
    leaves=[]
    for path in INPUTS:
        entry=current[path];raw=git(root,'cat-file','blob',entry['blob'])
        leaves.append(dict(path=path,**entry,sha256=digest(raw),size=len(raw)))
    def content(commit,path):return git(root,'show',commit+':'+path)
    anonymous=b'  // The source: live pages of anonymous regions.'
    before=content(BASE,'compat/posix/src/mman.cc');after=content(source,'compat/posix/src/mman.cc')
    require(before[before.index(anonymous):]==after[after.index(anonymous):],'anonymous algorithm changed')
    root_before=content(BASE,'CMakeLists.txt');root_after=content(source,'CMakeLists.txt')
    require(root_after==root_before+ROOT_BLOCK,'root block not exactly removable')
    test='tests/native2/native2_test.cpp'
    require(old[test]==current[test],'native2 baseline changed')
    return dict(schema=1,scope=SCOPE,source=source,tree=git(root,'rev-parse',source+'^{tree}').decode().strip(),base=BASE,
                inventory_sha256=digest(canonical(current)),inputs=leaves,anonymous_suffix_sha256=digest(before[before.index(anonymous):]),native2_blob=old[test]['blob'])
def validate_freeze(value):
    keys(value,('schema','scope','source','tree','base','inventory_sha256','inputs','anonymous_suffix_sha256','native2_blob'))
    require(type(value['schema'])is int and value['schema']==1 and value['scope']==SCOPE and value['base']==BASE,'freeze scope')
    for field in ('source','tree','native2_blob'):hexid(value[field],40)
    for field in ('inventory_sha256','anonymous_suffix_sha256'):hexid(value[field],64)
    require(type(value['inputs'])is list and [v['path']for v in value['inputs']]==list(INPUTS),'input set/order')
    for row in value['inputs']:
        keys(row,('path','mode','blob','sha256','size'));hexid(row['blob'],40);hexid(row['sha256'],64)
        require(row['mode']in ('100644','100755')and type(row['size'])is int and 0<row['size']<=16*1024*1024,'input mode/size')
    return value
CONTEXT_FIELDS=('source','run','attempt','repository','repository_id')
def validate_context(value):
    keys(value,CONTEXT_FIELDS);hexid(value['source'],40)
    require(value['repository']==REPO and value['repository_id']==RID,'repository identity')
    for field in ('run','attempt'):require(type(value[field])is str and re.fullmatch('[1-9][0-9]*',value[field]),'run/attempt')
    return value
def current_context(env,job):
    expected={'GITHUB_ACTIONS':'true','GITHUB_REPOSITORY':REPO,'GITHUB_REPOSITORY_ID':RID,
      'GITHUB_REF':'refs/heads/'+BRANCH,'GITHUB_WORKFLOW':WORKFLOW,
      'GITHUB_WORKFLOW_REF':REPO+'/'+CALLER+'@refs/heads/'+BRANCH,'GITHUB_JOB':job,
      'GITHUB_ACTOR':'palmoni5','GITHUB_TRIGGERING_ACTOR':'palmoni5'}
    require(all(env.get(k)==v for k,v in expected.items()),'wrong dedicated caller')
    require(env.get('GITHUB_EVENT_NAME')in ('push','workflow_dispatch'),'event')
    source=env.get('GITHUB_SHA');hexid(source,40)
    require(source==env.get('GITHUB_WORKFLOW_SHA')==env.get('GESHER_FILE_MREMAP_AUTHORIZED_SOURCE'),'caller/source authority')
    return validate_context(dict(source=source,run=env.get('GITHUB_RUN_ID'),attempt=env.get('GITHUB_RUN_ATTEMPT'),repository=REPO,repository_id=RID))
def validate_plan(plan,expected,source_freeze):
    """expected MUST come from separate immutable policy/current API, not plan.

    Structural success is not a permission token. The eventual caller gate
    must additionally prove its pinned G/blob/source-DAG and authenticated
    current run/jobs/artifacts before invoking a native consumer.
    """
    keys(expected,('context','policy_commit','policy_blob','source_manifest_sha256','whole_peer_sha256'))
    validate_context(expected['context'])
    for name in ('policy_commit','policy_blob'):hexid(expected[name],40)
    for name in ('source_manifest_sha256','whole_peer_sha256'):hexid(expected[name],64)
    validate_freeze(source_freeze)
    require(digest(canonical(source_freeze))==expected['source_manifest_sha256'],'unbound source freeze')
    require(source_freeze['source']==expected['context']['source'],'different source')
    keys(plan,('schema','scope','context','policy_ref','policy_blob','whole_peer_sha256','source_manifest_sha256','producer_job','reference_job','consumers'))
    require(type(plan['schema'])is int and plan['schema']==1 and plan['scope']==SCOPE,'plan scope')
    require(plan['context']==expected['context'],'stale/foreign attempt')
    require(plan['policy_ref']==REPO+'/'+POLICY+'@'+expected['policy_commit'],'policy pin')
    for name in ('policy_blob','whole_peer_sha256','source_manifest_sha256'):require(plan[name]==expected[name],'unbound '+name)
    require(plan['producer_job']=='produce'and plan['reference_job']=='linux-reference','producer/reference role')
    require(plan['consumers']==['windows-2022','windows-2025'],'consumer host set')
    return plan
