"""Append the reviewed source-only resource primitive delta; no compilation."""
from pathlib import Path
import difflib, hashlib, json, re, subprocess

ROOT=Path(__file__).resolve().parents[2]
OLD=Path('C:/work/gesher-pthread-reaper-expanded')
NEW=Path('C:/work/gesher-pthread-resource-expanded')
patch=ROOT/'tools/host-pthread-authority/supplier.patch'
base=subprocess.check_output(['git','show','09c34802:tools/host-pthread-authority/supplier.patch'],cwd=ROOT)
assert patch.read_bytes()==base
pieces=[]
for name in ('thread.c','thread.h'):
    rel='mingw-w64-libraries/winpthreads/src/'+name
    before=(OLD/rel).read_text(); after=(NEW/rel).read_text()
    raw=list(difflib.unified_diff(before.splitlines(keepends=True),after.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=2))
    assert raw
    out=raw[:2]; i=2
    while i<len(raw):
        m=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)\n',raw[i]); assert m
        a,ac,b,bc=int(m[1]),int(m[2] or 1),int(m[3]),int(m[4] or 1); tail=m[5]; i+=1; body=[]
        while i<len(raw) and not raw[i].startswith('@@ '): body.append(raw[i]); i+=1
        body=['\n' if line==' \n' else line for line in body]
        out.append(f'@@ -{a},{ac} +{b},{bc} @@{tail}\n'); out.extend(body)
    pieces.append(''.join(out))
patch.write_bytes(base+''.join(pieces).encode())
pin=ROOT/'tools/host-pthread-authority/pinned-inputs.json'
data=json.loads(pin.read_text()); data['patch_sha256']=hashlib.sha256(patch.read_bytes()).hexdigest()
pin.write_text(json.dumps(data,indent=2)+'\n',newline='\n')
print('Generated source-only resource lease experiment')
