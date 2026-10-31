"""Bounded CI API transport and current context; requires authenticated code.

This is a building block, not an immutable caller or complete authorization.
"""
import json
import hashlib
import os
import re
import urllib.request
import urllib.parse

def context(env,job):
    assert env.get('GITHUB_ACTIONS')=='true' and env.get('GITHUB_JOB')==job
    assert env.get('GITHUB_REPOSITORY')=='gesher-runtime/gesher' and env.get('GITHUB_REPOSITORY_ID')=='1426153186'
    assert env.get('GITHUB_REF')=='refs/heads/wip/ci-graphics-processes'
    assert env.get('GITHUB_ACTOR')==env.get('GITHUB_TRIGGERING_ACTOR')=='palmoni5'
    assert env.get('GITHUB_EVENT_NAME') in ('push','workflow_dispatch')
    source=env.get('GITHUB_SHA','');assert re.fullmatch('[0-9a-f]{40}',source)
    assert env.get('GITHUB_WORKFLOW_SHA')==source
    for key in ('GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'):
        assert re.fullmatch('[1-9][0-9]{0,19}',env.get(key,''))
    return {'source':source,'run':env['GITHUB_RUN_ID'],'attempt':env['GITHUB_RUN_ATTEMPT']}

def api(path):
    assert os.environ.get('GITHUB_ACTIONS')=='true'
    assert type(path)is str and re.fullmatch(r'actions/[A-Za-z0-9/?=&._-]+',path)
    request=urllib.request.Request('https://api.github.com/repos/gesher-runtime/gesher/'+path,
        headers={'Authorization':'Bearer '+os.environ['GH_TOKEN'],'Accept':'application/vnd.github+json'})
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self,*args,**kwargs):raise ValueError('unexpected redirect')
    with urllib.request.build_opener(NoRedirect()).open(request,timeout=30) as response:
        raw=response.read(4*1024*1024+1)
    assert len(raw)<=4*1024*1024
    def pairs(rows):
        value={}
        for k,v in rows:assert k not in value;value[k]=v
        return value
    return json.loads(raw,object_pairs_hook=pairs,parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite')))

def download_artifact(artifact_id,expected):
    assert os.environ.get('GITHUB_ACTIONS')=='true' and type(artifact_id)is int and artifact_id>0
    assert type(expected)is str and re.fullmatch('[0-9a-f]{64}',expected)
    class Redirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self,req,fp,code,msg,headers,newurl):
            p=urllib.parse.urlparse(newurl)
            assert p.scheme=='https' and p.hostname and p.port in (None,443) and p.username is None and p.password is None
            assert p.hostname in ('github.com','api.github.com','objects.githubusercontent.com','release-assets.githubusercontent.com') or p.hostname.endswith('.blob.core.windows.net')
            result=super().redirect_request(req,fp,code,msg,headers,newurl)
            if result is not None:result.remove_header('Authorization')
            return result
    url='https://api.github.com/repos/gesher-runtime/gesher/actions/artifacts/'+str(artifact_id)+'/zip'
    request=urllib.request.Request(url,headers={'Authorization':'Bearer '+os.environ['GH_TOKEN']})
    with urllib.request.build_opener(Redirect()).open(request,timeout=60) as response:raw=response.read(1048577)
    assert len(raw)<=1048576 and hashlib.sha256(raw).hexdigest()==expected
    return raw
