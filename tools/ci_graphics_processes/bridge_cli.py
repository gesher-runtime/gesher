"""CI bridge entrypoint. A pinned bootstrap must authenticate before loading."""
import json
import hashlib
import os
from pathlib import Path
import sys
import types

def load(name):
    p=Path(__file__).with_name(name+'.py');m=types.ModuleType(name);m.__file__=str(p)
    exec(compile(p.read_bytes(),str(p),'exec'),m.__dict__);return m

def main():
    assert len(sys.argv)==2 and sys.argv[1] in ('produce','consume')
    mode=sys.argv[1];job='fixture-producer' if mode=='produce' else 'fixture'
    pins=json.loads(os.environ['GRAPHICSDEX_PINS'])
    guard=os.environ['GRAPHICSDEX_GUARD'];reviewed=os.environ['GRAPHICSDEX_CALLER']
    b=load('bridge');m=load('manifest');env=load('api_context')
    current,_,jobs=b.authenticated(Path.cwd(),job,pins,guard,reviewed)
    if mode=='produce':
        value=b.producer_manifest(Path.cwd(),current,jobs,guard)
        dest=Path(os.environ['RUNNER_TEMP'])/'graphicsdex-source-manifest.json'
        with dest.open('xb')as f:f.write(m.canonical(value)+b'\n')
        print('GRAPHICSDEX_PRODUCER '+json.dumps({'context':value['context'],'producer_id':value['producer_id'],'manifest_sha256':hashlib.sha256(m.canonical(value)).hexdigest(),'qualification':False},sort_keys=True))
    else:
        inventory=env.api('actions/runs/'+current['run']+'/artifacts?per_page=100')
        artifact,producer_id=b.consumer_artifact(current,jobs,inventory)
        print('GRAPHICSDEX_ARTIFACT '+json.dumps({'context':b.manifest_context(current,guard),'producer_id':producer_id,'artifact_id':artifact['id'],'digest':artifact['digest'],'qualification':False},sort_keys=True))
        archive=env.download_artifact(artifact['id'],artifact['digest'].split(':',1)[1])
        digest=b.consumer_manifest(Path.cwd(),archive,current,guard,producer_id)
        print('GRAPHICSDEX_MANIFEST '+json.dumps({'manifest_sha256':digest,'qualification':False},sort_keys=True))

if __name__=='__main__':main()
