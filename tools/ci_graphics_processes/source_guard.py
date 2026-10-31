"""Source inventory predicates for prospective GraphicsDex authority.

These predicates alone do not authenticate a caller or trigger CI.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

BASE='b0de972bb543e0dd20d125c990817fb5b3407112'
BASE_INVENTORY='5c15755b5e6223b64305947c610bf3de69adb1edf03ef9dd2b690788ca20a905'
CALLER='.github/workflows/graphicsdex-component.yml'
GUARD='.github/workflows/graphicsdex-trusted-source.yml'
PYTHON=('tools/ci_graphics_processes/manifest.py','tools/ci_graphics_processes/test_manifest.py',
        'tools/ci_graphics_processes/role_checks.py','tools/ci_graphics_processes/test_role_checks.py',
        'tools/ci_graphics_processes/source_guard.py','tools/ci_graphics_processes/test_source_guard.py',
        'tools/ci_graphics_processes/api_context.py','tools/ci_graphics_processes/test_api_context.py')
RUNNER='tools/ci_graphics_processes/run-fixture.ps1'
ADDITIONS=(CALLER,GUARD,*PYTHON,RUNNER)

def canonical(v):return (json.dumps(v,sort_keys=True,separators=(',',':'))+'\n').encode()
def inventory(root,ref):
    raw=subprocess.check_output(['git','-C',str(root),'ls-tree','-r','-z',ref]);rows={}
    for record in raw.split(b'\0'):
        if not record:continue
        meta,name=record.split(b'\t',1);mode,kind,blob=meta.decode().split();name=name.decode()
        assert kind=='blob' and mode in ('100644','100755') and name not in rows
        rows[name]={'mode':mode,'blob':blob}
    return rows

def verify_inventory(base,current,pins):
    assert hashlib.sha256(canonical(base)).hexdigest()==BASE_INVENTORY
    assert not set(base)&set(ADDITIONS)
    assert set(current)==set(base)|set(ADDITIONS) and set(pins)==set(ADDITIONS)
    assert all(current[p]==v for p,v in base.items())
    assert all(current[p]=={'mode':'100644','blob':pins[p]} for p in ADDITIONS)

def verify_checkout(root,source,pins):
    root=Path(root)
    assert type(source)is str and re.fullmatch('[0-9a-f]{40}',source)
    def git(*args):return subprocess.check_output(['git','-C',str(root),*args])
    assert git('rev-parse','HEAD').decode().strip()==source
    assert subprocess.run(['git','-C',str(root),'merge-base','--is-ancestor',BASE,source],capture_output=True).returncode==0
    current=inventory(root,source);verify_inventory(inventory(root,BASE),current,pins)
    # Read actual working bytes, avoiding index/stat-cache based assertions.
    for name,row in current.items():
        path=root/name;assert path.is_file() and not path.is_symlink()
        raw=path.read_bytes()
        actual=hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()
        assert actual==row['blob']
    assert not git('status','--porcelain','--untracked-files=all').strip()
    return current
