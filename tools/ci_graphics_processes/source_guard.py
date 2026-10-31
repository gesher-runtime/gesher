"""Source inventory predicates for prospective GraphicsDex authority.

These predicates alone do not authenticate a caller or trigger CI.
"""
import hashlib
import json
import subprocess

BASE='b0de972bb543e0dd20d125c990817fb5b3407112'
BASE_INVENTORY='5c15755b5e6223b64305947c610bf3de69adb1edf03ef9dd2b690788ca20a905'
CALLER='.github/workflows/graphicsdex-component.yml'
GUARD='.github/workflows/graphicsdex-trusted-source.yml'
PYTHON=('tools/ci_graphics_processes/manifest.py','tools/ci_graphics_processes/test_manifest.py',
        'tools/ci_graphics_processes/role_checks.py','tools/ci_graphics_processes/test_role_checks.py',
        'tools/ci_graphics_processes/source_guard.py','tools/ci_graphics_processes/test_source_guard.py')
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
