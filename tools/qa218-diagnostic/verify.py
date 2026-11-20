#!/usr/bin/env python3
"""Strict baseline/observation binding; never normalizes source bytes."""
import argparse, hashlib, json, os, pathlib, re, subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
BASE='fea065241a5c2589d839c94fc1de579c2d02c838'
BRANCH='refs/heads/qa/diagnostic-pr218-geocoder-preload-20261120'
PINS={
 'tests/windows/tracks/geocoder-operation.ps1':'75eb4cf6f9537c61ff27ec49d2a3a938c81b1618',
 'tests/windows/tracks/geocoder-probe-exit.cs':'4927e87a71c41069bbed24be748cfd0b771583d5',
 'tests/windows/tracks/geocoder-winrt-supplier.cs':'11da9f04efdbb98de30006b0c26e169bec6d8b19',
 'tests/windows/helpers/geocoder-provider-probe.ps1':'d02fc316ccaca1ac856eaeae64df6e4053da569c',
 'tests/windows/helpers/geocoder-winrt-fixture.ps1':'cbde2ac48b603f3cc12e544a3e42d744d9b58864',
 'tests/windows/helpers/geocoder-roslyn-worker.ps1':'31491e2cb20bf72e78c49476c9ab874649fee6c9',
 'tests/windows/helpers/geocoder-compiler-host.ps1':'43f5fc28e1c0a19e8d8c5e1df1a3d823f734f16f',
 'tests/windows/helpers/geocoder-compile.ps1':'90fceedc449a2ec11dfa80809e05163c89bcf2fa',
 'tests/windows/helpers/run-geocoder-winrt-component.ps1':'1277bcc858c9c52eb8b24476831aadb286831418',
 'tests/windows/tracks/58-devices.ps1':'b3f3f90ccd1c4c4cc849def7bcdceac03b83be4e',
 '.github/workflows/services-geocoder-worker-component.yml':'0bd90d14e361816c3fca75037a7b45e7a94f3ff9',
}
OVERLAYS={'tests/windows/tracks/geocoder-operation.ps1','tests/windows/helpers/geocoder-winrt-fixture.ps1'}
ADDED={'tools/qa218-diagnostic/run.ps1','tools/qa218-diagnostic/verify.py','.github/workflows/qa218-geocoder-preload.yml'}
CONTROL=['tools/windows4/TsfJobSupervisor.cs','tools/windows4/test-tsf-job-supervisor.ps1','tools/windows4/diagnose-pwsh-job.ps1','tests/windows/tracks/geocoder-operation-fixture.ps1']
PATTERN=re.compile(rb'^# QA218-OBSERVATION-BEGIN ([a-z0-9-]+)\n.*?^# QA218-OBSERVATION-END \1\n',re.M|re.S)
def git(*a): return subprocess.check_output(['git','-C',str(ROOT),*a])
def blob(b): return hashlib.sha1(('blob '+str(len(b))+'\0').encode()+b).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--check-only',action='store_true');a=p.parse_args()
 head=git('rev-parse','HEAD').decode().strip()
 if not a.check_only:
  assert os.environ.get('GITHUB_ACTIONS')=='true' and os.environ.get('GITHUB_REPOSITORY')=='gesher-runtime/gesher'
  assert head==os.environ['GITHUB_SHA'] and os.environ['GITHUB_REF']==BRANCH
 assert git('rev-parse','HEAD^').decode().strip()==BASE
 assert not git('status','--porcelain=v1','--untracked-files=no')
 changed=set(git('diff','--name-only',BASE,head).decode().splitlines())
 assert changed==OVERLAYS|ADDED, sorted(changed)
 assert git('show',BASE+':.gitattributes')==b'* text=auto eol=lf\n'
 records=[]
 for path in list(PINS)+CONTROL+sorted(ADDED):
  actual=(ROOT/path).read_bytes()
  assert blob(actual)==git('rev-parse',head+':'+path).decode().strip(), path
  rec={'path':pathlib.PurePosixPath(path).as_posix(),'blob':blob(actual),'sha256':hashlib.sha256(actual).hexdigest(),'bytes':len(actual)}
  if path not in ADDED:
   baseline=git('show',BASE+':'+path);rec['baseline_blob']=blob(baseline);rec['baseline_sha256']=hashlib.sha256(baseline).hexdigest()
   if path in PINS: assert blob(baseline)==PINS[path],path
   if path in OVERLAYS:
    assert b'\r' not in actual and PATTERN.sub(b'',actual)==baseline,path
    rec['observation_blocks']=[x.decode() for x in PATTERN.findall(actual)]
    assert rec['observation_blocks'] and len(rec['observation_blocks'])==len(set(rec['observation_blocks']))
   else: assert actual==baseline,path
  records.append(rec)
 manifest={'repository':'gesher-runtime/gesher','source':head,'tree':git('rev-parse','HEAD^{tree}').decode().strip(),'base':BASE,'run':os.environ.get('GITHUB_RUN_ID'),'attempt':os.environ.get('GITHUB_RUN_ATTEMPT'),'sources':records,'dll_sha256':'deec6ef8ebf0c6b8e8e405d1b65690ee290a54941290c06765bc960382544a14','runtime_deadline_ms':45000,'compiler_cancellation_ms':40000,'original_runtime_supervisor_assertions':'byte-identical; includes all handle/retirement/output/case checks'}
 if not a.check_only:
  root=pathlib.Path(os.environ['GESHER_QA218_RESULTS']);root.mkdir(parents=False,exist_ok=False)
  (root/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 print(json.dumps(manifest,indent=2))
if __name__=='__main__': main()
