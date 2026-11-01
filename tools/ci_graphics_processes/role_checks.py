"""Pure prospective API role checks. Not a source guard or API client."""
import datetime
import re

REPOSITORY='gesher-runtime/gesher'
REPOSITORY_ID=1426153186
CALLER='.github/workflows/graphicsdex-component.yml'
GUARD='.github/workflows/graphicsdex-trusted-source.yml'
BRANCH='wip/ci-graphics-processes'
WORKFLOW='graphicsdex-component'

def verify_run(run,current,guard):
    assert str(run['id'])==current['run'] and str(run['run_attempt'])==current['attempt'] and run['head_sha']==current['source']
    assert run['name']==WORKFLOW and run['path']==CALLER and run['head_branch']==BRANCH and run['event'] in ('push','workflow_dispatch')
    assert run['actor']['login']==run['triggering_actor']['login']=='palmoni5'
    for key in ('repository','head_repository'):
        assert run[key]['id']==REPOSITORY_ID and run[key]['full_name']==REPOSITORY
    refs=run['referenced_workflows']
    assert type(refs)is list and len(refs)==1 and refs[0]['path']==REPOSITORY+'/'+GUARD+'@'+guard and refs[0]['sha']==guard

def verify_jobs(inventory,current,names):
    jobs=inventory['jobs'];assert inventory['total_count']==len(jobs)<=100
    result={}
    for name in names:
        matches=[job for job in jobs if job['name']==name];assert len(matches)==1
        job=matches[0]
        assert job['head_sha']==current['source'] and str(job['run_id'])==current['run'] and str(job['run_attempt'])==current['attempt']
        assert job['status']=='completed' and job['conclusion']=='success' and type(job['id'])is int and job['id']>0
        result[name]=job
    return result

def verify_artifact(inventory,current,producer):
    artifacts=inventory['artifacts'];assert inventory['total_count']==len(artifacts)<=100
    name='graphicsdex-source-'+current['source']+'-'+current['run']+'-'+current['attempt']
    found=[x for x in artifacts if x['name']==name];assert len(found)==1
    value=found[0];run=value['workflow_run']
    assert type(value['id'])is int and value['id']>0 and value['expired'] is False
    assert type(value['size_in_bytes'])is int and 0<value['size_in_bytes']<=1048576
    assert type(value.get('digest'))is str and re.fullmatch('sha256:[0-9a-f]{64}',value['digest'])
    assert str(run['id'])==current['run'] and run['head_sha']==current['source'] and run['repository_id']==run['head_repository_id']==REPOSITORY_ID
    def stamp(v):return datetime.datetime.fromisoformat(v.replace('Z','+00:00'))
    assert stamp(producer['started_at'])<=stamp(value['created_at'])<=stamp(producer['completed_at'])
    return value
