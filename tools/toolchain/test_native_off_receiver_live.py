"""Pure identity/control metadata: never execute compiler/native/network."""
import copy
import native_off_receiver as n
import native_off_receiver_context as c

ctx={'GITHUB_SHA':'1'*40,'GITHUB_RUN_ID':'123','GITHUB_RUN_ATTEMPT':'1',
     'GITHUB_REPOSITORY':c.REPOSITORY,'GITHUB_REPOSITORY_ID':c.REPOSITORY_ID}
policy=c.REPOSITORY+'/'+c.POLICY_PATH+'@'+'2'*40
run=dict(id=123,run_attempt=1,head_sha='1'*40,head_branch=c.BRANCH,path=c.PATH,name=c.WORKFLOW,event='push',
         repository=dict(full_name=c.REPOSITORY,id=int(c.REPOSITORY_ID)),head_repository=dict(full_name=c.REPOSITORY,id=int(c.REPOSITORY_ID)),
         actor=dict(login='palmoni5'),triggering_actor=dict(login='palmoni5'),referenced_workflows=[dict(path=policy,sha='2'*40)])
def jobs(phase):
    rows=[]
    for i,name in enumerate(sorted(n.NAMES)):
        status,conclusion='queued',None
        if name=='source_authority / verify':status,conclusion='completed','success'
        if name in ('build','native3-locks-reference'):
            if phase=='receiver':status,conclusion='completed','success'
            elif name==('build'if phase=='build'else'native3-locks-reference'):status='in_progress'
        rows.append(dict(id=i+1,name=name,head_sha='1'*40,run_id=123,run_attempt=1,status=status,conclusion=conclusion))
    return dict(total_count=len(rows),jobs=rows)
negatives=0
def reject(r,j,phase):
    global negatives
    try:n.validate_live(r,j,ctx,policy,'push',phase)
    except (AssertionError,KeyError):negatives+=1
    else:raise AssertionError('Wrong live producer/authority accepted')
for phase in ('build','reference','receiver'):
    original=jobs(phase);n.validate_live(run,original,ctx,policy,'push',phase)
    for field,value in [('id',0),('run_attempt',2),('head_sha','0'*40),('head_branch','main'),('path','.github/workflows/ci.yml'),('name','ci'),('event','schedule'),('referenced_workflows',[])]:
        r=copy.deepcopy(run);r[field]=value;reject(r,original,phase)
    for field in ('actor','triggering_actor'):
        r=copy.deepcopy(run);r[field]['login']='other';reject(r,original,phase)
    for field in ('repository','head_repository'):
        for key,value in [('id',0),('full_name','other/repo')]:
            r=copy.deepcopy(run);r[field][key]=value;reject(r,original,phase)
    for field,value in [('head_sha','0'*40),('run_id',0),('run_attempt',2)]:
        j=copy.deepcopy(original);j['jobs'][0][field]=value;reject(run,j,phase)
    j=copy.deepcopy(original);j['total_count']+=1;reject(run,j,phase)
    j=copy.deepcopy(original);j['jobs'][0]['name']='unexpected';reject(run,j,phase)
    for name in ['source_authority / verify']+(['build','native3-locks-reference']if phase=='receiver'else[]):
        j=copy.deepcopy(original);j['jobs']=[x for x in j['jobs']if x['name']!=name];j['total_count']=len(j['jobs']);reject(run,j,phase)
        j=copy.deepcopy(original);next(x for x in j['jobs']if x['name']==name)['conclusion']='failure';reject(run,j,phase)
print('NATIVE_OFF_LIVE_PURE_PASS phases3 negatives='+str(negatives)+' compiler_native_network0')
