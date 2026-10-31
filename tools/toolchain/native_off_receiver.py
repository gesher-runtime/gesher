"""Exact CI-only ordinary OFF Native role coordinator; no runtime acceptance."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import native_off_receiver_context as focused

ROOT=Path(__file__).resolve().parents[2]
NAMES={'source_authority / verify','build','native3-locks-reference',
       'native3-locks-regression-windows-2022','native3-locks-regression-windows-2025'}

def policy_ref():
    text=(ROOT/focused.PATH).read_text(encoding='utf-8')
    pins=re.findall(r'^\s*uses: ('+re.escape(focused.REPOSITORY+'/'+focused.POLICY_PATH)+r'@[0-9a-f]{40})\s*$',text,re.M)
    assert len(pins)==1
    return pins[0]

def validate_live(run,jobs,context,policy,event,phase):
    assert str(run['id'])==context['GITHUB_RUN_ID'] and str(run['run_attempt'])==context['GITHUB_RUN_ATTEMPT']
    assert run['head_sha']==context['GITHUB_SHA'] and run['head_branch']==focused.BRANCH
    assert run['path']==focused.PATH and run['name']==focused.WORKFLOW and run['event']==event
    assert run['repository']['full_name']==run['head_repository']['full_name']==focused.REPOSITORY
    assert str(run['repository']['id'])==str(run['head_repository']['id'])==focused.REPOSITORY_ID
    assert run['actor']['login']==run['triggering_actor']['login']=='palmoni5'
    refs=[r for r in run['referenced_workflows'] if r['path']==policy]
    assert len(refs)==1 and refs[0]['sha']==policy.rsplit('@',1)[1]
    rows=jobs['jobs'];assert jobs['total_count']==len(rows)<=100
    assert len({r['name']for r in rows})==len(rows) and {r['name']for r in rows}<=NAMES
    assert all(r['head_sha']==context['GITHUB_SHA'] and str(r['run_id'])==context['GITHUB_RUN_ID'] and str(r['run_attempt'])==context['GITHUB_RUN_ATTEMPT']for r in rows)
    needed=['source_authority / verify']
    if phase=='receiver':needed+=['build','native3-locks-reference']
    for name in needed:
        selected=[r for r in rows if r['name']==name];assert len(selected)==1
        assert selected[0]['status']=='completed' and selected[0]['conclusion']=='success'
    if phase in ('build','reference'):
        name='build'if phase=='build'else 'native3-locks-reference'
        own=[r for r in rows if r['name']==name];assert len(own)==1
        assert own[0]['status']=='in_progress' and own[0]['conclusion']is None

def current(phase):
    assert os.environ.get('GITHUB_ACTIONS')=='true'
    context=(focused.context(os.environ,'selected-minimum')if phase=='build'else
             focused.reference_context(os.environ)if phase=='reference'else focused.consumer_context(os.environ,'selected-minimum'))
    assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()==context['GITHUB_SHA']
    assert not subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=ROOT)
    policy=policy_ref();assert policy==os.environ['GESHER_NATIVE_OFF_RECEIVER_POLICY_REF']
    def api(path):return json.loads(subprocess.check_output(['gh','api','repos/'+focused.REPOSITORY+'/'+path],text=True))
    run=api('actions/runs/'+context['GITHUB_RUN_ID'])
    jobs=api('actions/runs/'+context['GITHUB_RUN_ID']+'/attempts/'+context['GITHUB_RUN_ATTEMPT']+'/jobs?per_page=100')
    print('NATIVE_OFF_CURRENT_ROLE '+json.dumps(dict(expected=dict(context=context,policy=policy,phase=phase),received=dict(run={k:run.get(k)for k in ('id','run_attempt','head_sha','head_branch','path','name','event','actor','triggering_actor','referenced_workflows')},jobs=[{k:r.get(k)for k in ('id','name','head_sha','run_id','run_attempt','status','conclusion')}for r in jobs['jobs']])),sort_keys=True),flush=True)
    validate_live(run,jobs,context,policy,os.environ['GITHUB_EVENT_NAME'],phase)
    return context

def main():
    p=argparse.ArgumentParser();p.add_argument('phase',choices=('build','reference','receiver'));args=p.parse_args()
    current(args.phase)
    print('NATIVE_OFF_SCOPE genuine_OFF_producer+fresh_reference+receiver_controls_and_assembly_ONLY runtime161_executed=false canonical_adoption=false full_qualification=false',flush=True)

if __name__=='__main__':main()
