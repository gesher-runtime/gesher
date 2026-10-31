"""Pure live-API identity fixture controls; no API request or native command."""
import copy
import first_link as component
import first_link_context as focused


def main():
    ctx=dict(GITHUB_SHA='1'*40,GITHUB_RUN_ID='123456',GITHUB_RUN_ATTEMPT='1',GITHUB_REPOSITORY=focused.REPOSITORY,GITHUB_REPOSITORY_ID=focused.REPOSITORY_ID)
    policy=focused.REPOSITORY+'/'+focused.POLICY_PATH+'@'+'2'*40
    run=dict(id=123456,run_attempt=1,head_sha=ctx['GITHUB_SHA'],head_branch=focused.BRANCH,path=focused.PATH,name=focused.WORKFLOW,event='push',
             repository=dict(id=int(focused.REPOSITORY_ID),full_name=focused.REPOSITORY),head_repository=dict(id=int(focused.REPOSITORY_ID)),
             actor=dict(login='palmoni5'),triggering_actor=dict(login='palmoni5'),referenced_workflows=[dict(path=policy,sha='2'*40)])
    def job(name,status='completed',conclusion='success'):
        return dict(id=7,name=name,head_sha=ctx['GITHUB_SHA'],run_id=123456,run_attempt=1,status=status,conclusion=conclusion)
    jobs=dict(total_count=2,jobs=[job('source_authority / verify'),job('build','in_progress',None)])
    component.validate_live(run,jobs,ctx,policy,'push')
    failures=[]
    for field,value in [('id',9),('run_attempt',2),('head_sha','3'*40),('head_branch','main'),('path','.github/workflows/ci.yml'),('name','ci'),('event','pull_request')]:
        a=copy.deepcopy(run);a[field]=value;failures.append((a,jobs))
    for object_name,field,value in [('repository','id',1),('repository','full_name','other/gesher'),('head_repository','id',1),('actor','login','other'),('triggering_actor','login','other')]:
        a=copy.deepcopy(run);a[object_name][field]=value;failures.append((a,jobs))
    a=copy.deepcopy(run);a['referenced_workflows']=[];failures.append((a,jobs))
    a=copy.deepcopy(run);a['referenced_workflows'][0]['sha']='3'*40;failures.append((a,jobs))
    a=copy.deepcopy(run);a['referenced_workflows']*=2;failures.append((a,jobs))
    for field,value in [('name','old-policy'),('run_attempt',2),('run_id',9),('head_sha','3'*40),('status','in_progress'),('conclusion','failure')]:
        b=copy.deepcopy(jobs);b['jobs'][0][field]=value;failures.append((run,b))
    b=copy.deepcopy(jobs);b['jobs'].pop(0);b['total_count']=1;failures.append((run,b))
    b=copy.deepcopy(jobs);b['jobs'].append(job('full-art-producer'));b['total_count']=3;failures.append((run,b))
    b=copy.deepcopy(jobs);b['jobs'].append(copy.deepcopy(b['jobs'][0]));b['total_count']=3;failures.append((run,b))
    b=copy.deepcopy(jobs);b['total_count']=3;failures.append((run,b))
    for field,value in [('run_attempt',2),('head_sha','3'*40),('status','queued'),('conclusion','failure')]:
        b=copy.deepcopy(jobs);b['jobs'][1][field]=value;failures.append((run,b))
    b=copy.deepcopy(jobs);extra=job('portable-windows-2022','queued',None);extra['run_attempt']=2;b['jobs'].append(extra);b['total_count']=3;failures.append((run,b))
    for a,b in failures:
        try:component.validate_live(a,b,ctx,policy,'push')
        except (AssertionError,KeyError):pass
        else:raise AssertionError('Wrong current source/policy/producer API identity accepted')
    consumer=copy.deepcopy(jobs);consumer['jobs'][1].update(status='completed',conclusion='success')
    component.validate_live(run,consumer,ctx,policy,'push',consumer=True)
    consumer['jobs'][1]['conclusion']='failure'
    try:component.validate_live(run,consumer,ctx,policy,'push',consumer=True)
    except AssertionError:pass
    else:raise AssertionError('Failed focused producer accepted by portable consumer')
    print('PASS FIRST_LINK_ONLY liveAPI pure '+str(len(failures)+1)+' source/path/ref/actor/policy/currentattempt/role negatives; no API/native/network')


if __name__=='__main__':main()
