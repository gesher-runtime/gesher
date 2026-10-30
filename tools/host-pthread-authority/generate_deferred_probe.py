"""Generate only the isolated diagnostic capability delta, never the old patches."""
from pathlib import Path
import argparse,difflib,hashlib,json
ROOT=Path(__file__).resolve().parents[2]
OLD=Path('C:/work/gesher-pthread-reboot-role-stack-check/probe-resource')
parser=argparse.ArgumentParser()
parser.add_argument('--new-source',type=Path,required=True)
NEW=parser.parse_args().new_source
here=ROOT/'tools/host-pthread-authority'
pin=json.loads((here/'pinned-inputs.json').read_text())
assert hashlib.sha256((here/'supplier.patch').read_bytes()).hexdigest()==pin['patch_sha256']
assert hashlib.sha256((here/'fault-supplier.patch').read_bytes()).hexdigest()==pin['fault_patch_sha256']
chunks=[]; outputs={}
for name in ('thread.c','thread.h'):
    rel='mingw-w64-libraries/winpthreads/src/'+name
    old=(OLD/rel).read_text(); new=(NEW/rel).read_text()
    chunks.extend(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+rel,tofile='b/'+rel,n=3))
    outputs[rel]=hashlib.sha256(new.encode()).hexdigest()
patch=here/'deferred-supplier.patch';patch.write_text(''.join(chunks),newline='\n')
pin['deferred_patch_sha256']=hashlib.sha256(patch.read_bytes()).hexdigest()
pin['deferred_source_sha256']=outputs
(here/'pinned-inputs.json').write_text(json.dumps(pin,indent=2)+'\n',newline='\n')
print('Generated deferred-only role delta; original patches unchanged')
