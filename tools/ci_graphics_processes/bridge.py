"""Prospective GraphicsDex role orchestration.

Call only from an independently authenticated immutable bootstrap that supplies
the exact pins, guard and reviewed caller. This module is not itself a guard.
"""
import os
from pathlib import Path
import types

def module(name):
    path=Path(__file__).with_name(name+'.py');value=types.ModuleType(name)
    exec(compile(path.read_bytes(),str(path),'exec'),value.__dict__)
    return value

def authenticated(root,job,pins,guard,reviewed):
    # A future immutable bootstrap must authenticate this file before execution.
    env=module('api_context');roles=module('role_checks');source=module('source_guard')
    current=env.context(os.environ,job)
    assert current['source']==reviewed
    source.verify_checkout(root,current['source'],pins)
    run=env.api('actions/runs/'+current['run']);roles.verify_run(run,current,guard)
    jobs=env.api('actions/runs/'+current['run']+'/attempts/'+current['attempt']+'/jobs?per_page=100')
    verified=roles.verify_jobs(jobs,current,['source-authority / verify'])
    return current,verified,jobs

def producer_job(jobs,current):
    assert jobs['total_count']==len(jobs['jobs'])<=100
    found=[x for x in jobs['jobs'] if x['name']=='fixture-producer'];assert len(found)==1
    job=found[0]
    assert job['head_sha']==current['source'] and str(job['run_id'])==current['run'] and str(job['run_attempt'])==current['attempt']
    assert job['status']=='in_progress' and job['conclusion'] is None and type(job['id'])is int and job['id']>0
    return job
