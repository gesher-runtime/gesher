"""Pure policy/metadata tests; old log receipt is never a new build proof."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import textwrap
import prepare_host_pthread as factory


def source_policy():
    import yaml
    root=Path(__file__).resolve().parents[2];base='1bf8fad9ec3b423ddef8c97c1d72b25790974364'
    def old(path):return yaml.safe_load(subprocess.check_output(['git','-C',str(root),'show',base+':'+path],text=True))
    prior=old('.github/workflows/ci.yml');current=yaml.safe_load((root/'.github/workflows/ci.yml').read_text())
    step=next(s for s in current['jobs']['build']['steps'] if s.get('uses')=='./.github/actions/setup-build')
    assert step['with'].pop('pthread-supplier')=="${{ github.ref == 'refs/heads/wip/pthread-production-factory' && 'selected-minimum' || 'publisher' }}"
    assert step['with'].pop('pthread-caller')=='ci/build';assert current==prior
    prior=old('.github/actions/setup-build/action.yml');current=yaml.safe_load((root/'.github/actions/setup-build/action.yml').read_text())
    assert current['inputs'].pop('pthread-supplier')['default']=='publisher'
    assert current['inputs'].pop('pthread-caller')['default']==''
    assert set(current.pop('outputs'))=={'selected_compiler_root','selected_receipt','selected_receipt_sha256','selected_recipe_fingerprint'}
    steps=current['runs']['steps'];selected=steps.pop(-2)
    assert selected['if']=="inputs.pthread-supplier == 'selected-minimum'" and selected['env']['GESHER_HOST_PTHREAD_CALLER']=='${{ inputs.pthread-caller }}'
    assert steps[:-1]==prior['runs']['steps'][:-1]
    configure=steps[-1]['run'];original=prior['runs']['steps'][-1]['run']
    otherwise=configure.split('else\n  test "$GESHER_HOST_PTHREAD_SELECTION" = publisher\n',1)[1].rsplit('\nfi',1)[0]
    assert textwrap.dedent(otherwise).strip()==original.split('\n',1)[1].strip()
    assert configure.splitlines()[0]==original.splitlines()[0]
    assert configure.index('sha256sum --check --strict')<configure.index('--verify "$receipt"')<configure.index('cmake --fresh')
    steps[-1]=prior['runs']['steps'][-1];assert current==prior
    print('PASS source-only first-ci/build opt-in; main/default publisher/cache steps and configure command exact; independent callers untouched')


def tests(old):
    env=dict(GITHUB_ACTIONS='true',GITHUB_REF='refs/heads/wip/pthread-production-factory',GITHUB_WORKFLOW='ci',GITHUB_JOB='build',
             GESHER_HOST_PTHREAD_SELECTION='selected-minimum',GESHER_HOST_PTHREAD_CALLER='ci/build',GITHUB_SHA='1'*40,GITHUB_RUN_ID='123456',GITHUB_RUN_ATTEMPT='1',
             GITHUB_REPOSITORY='gesher-runtime/gesher',GITHUB_REPOSITORY_ID='1426153186')
    ctx=factory.caller(env,'selected-minimum');rejected=0
    for key,value in [('GITHUB_ACTIONS','false'),('GITHUB_REF','refs/heads/main'),('GITHUB_JOB','compile'),('GITHUB_WORKFLOW','other'),('GESHER_HOST_PTHREAD_CALLER','other'),('GITHUB_REPOSITORY_ID','1'),('GESHER_HOST_PTHREAD_SELECTION','publisher-control'),('GITHUB_SHA','bad'),('GITHUB_RUN_ATTEMPT','0')]:
        bad=dict(env);bad[key]=value
        try:factory.caller(bad,'selected-minimum')
        except AssertionError:rejected+=1
        else:raise AssertionError('Implicit/unsupported production caller accepted')
    recipe=factory.recipe();fp=factory.fingerprint(recipe)
    changed=copy.deepcopy(recipe);changed['core_sources']['tools/host-pthread-authority/minimum-noattach-join.patch']='0'*64
    assert factory.fingerprint(changed)!=fp and factory.fingerprint(copy.deepcopy(recipe))==fp
    root='/metadata-only/gesher-host-pthread-selected-minimum-'+fp[:16];sdk=root+'/'+factory.NAME;source=root+'/source'
    old_sdk=old['compiler'].split('InstalledDir: ',1)[1].split('/bin',1)[0]
    old_source=next(iter(old['dll_symbol_objects'].values()))['actual_reading_path'].split('/build/')[0]
    def rewrite(value):
        if isinstance(value,dict):return {k:rewrite(v) for k,v in value.items()}
        if isinstance(value,list):return [rewrite(v) for v in value]
        if isinstance(value,str):return value.replace(old_sdk,sdk).replace(old_source,source)
        return value
    a=rewrite(old);a.update({k:ctx[k] for k in factory.FIELDS[:4]});a['role']=factory.minimum.ROLE;a['pin']=copy.deepcopy(factory.minimum.PIN)
    a['original']['context']={k:ctx[k] for k in factory.FIELDS[:4]}
    good=dict(schema='gesher-host-toolchain-v1',context=ctx,selection='selected-minimum',role=factory.minimum.ROLE,recipe=recipe,
              recipe_fingerprint=fp,workspace=root,compiler_root=sdk,source_root=source,source_commit=factory.minimum.PIN['upstream_commit'],
              source_before=copy.deepcopy(factory.minimum.PIN['source_sha256']),source_after=copy.deepcopy(factory.minimum.PIN['output_source_sha256']),
              archive=dict(url=factory.URL,sha256=factory.minimum.PIN['compiler_archive_sha256'],bytes=84003044),authority=a,
              authority_sha256=hashlib.sha256(json.dumps(a,sort_keys=True,indent=2).encode()).hexdigest())
    factory.check_metadata(good,ctx)
    control=copy.deepcopy(good);control['selection']='publisher-control';control['role']=factory.ROLES['publisher-control']
    control['workspace']='/metadata-only/gesher-host-pthread-publisher-control-'+fp[:16]
    control['compiler_root']=control['workspace']+'/'+factory.NAME;control['source_root']=control['workspace']+'/source'
    control['source_after']=copy.deepcopy(control['source_before']);control['authority']=copy.deepcopy(a['original'])
    control['authority_sha256']=hashlib.sha256(json.dumps(control['authority'],sort_keys=True,indent=2).encode()).hexdigest()
    factory.check_metadata(control,ctx)
    control_env=dict(env);control_env['GESHER_HOST_PTHREAD_SELECTION']='publisher-control'
    assert factory.caller(control_env,'publisher-control')==ctx
    mutations=[lambda x:x.update(selection='publisher-control'),lambda x:x.update(role='publisher-reference-unmodified-v1'),
        lambda x:x.update(recipe_fingerprint='0'*64),lambda x:x['recipe']['core_sources'].update({'tools/host-pthread-authority/minimum-inputs.json':'0'*64}),
        lambda x:x.update(workspace='/metadata-only/old-cache'),lambda x:x.update(compiler_root='/old/original'),
        lambda x:x['archive'].update(bytes=1),lambda x:x['authority'].update(GITHUB_SHA='0'*40),
        lambda x:x['authority'].update(private_record_layout=x['authority']['private_record_layout'].replace('sizeof=480','sizeof=496')),
        lambda x:x['authority']['original']['files'].update({'lib/libwinpthread.a':'0'*64}),
        lambda x:x['authority']['original_profile']['sdk'].update(_WIN32_WINNT=0xa00),
        lambda x:x['authority']['files'].update({'lib/libpthread.a':'0'*64}),
        lambda x:x['authority']['dll_symbol_objects'].pop('pthread_join'),
        lambda x:x['authority'].update(dll_compile_commands=[v.replace('-DDLL_EXPORT','') for v in x['authority']['dll_compile_commands']]),
        lambda x:x['authority']['build_logs'].pop('supplier-profile.obj'),
        lambda x:x['source_after'].update({'mingw-w64-libraries/winpthreads/src/clock.c':'0'*64})]
    for mutate in mutations:
        bad=copy.deepcopy(good);mutate(bad);bad['authority_sha256']=hashlib.sha256(json.dumps(bad['authority'],sort_keys=True,indent=2).encode()).hexdigest()
        try:factory.check_metadata(bad,ctx)
        except (AssertionError,KeyError):rejected+=1
        else:raise AssertionError('Invalid recipe/authority projection accepted')
    bad=copy.deepcopy(good);bad['authority_sha256']='0'*64
    try:factory.check_metadata(bad,ctx)
    except AssertionError:rejected+=1
    else:raise AssertionError('Authority digest mismatch accepted')
    print('PASS shared production interface caller/fingerprint/receipt negatives='+str(rejected)+' projection=SYNTHETIC_OLD_LOG_METADATA_ONLY native_processes=0 selected_build_claim=false')


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--diagnostic-receipt',type=Path,required=True);args=parser.parse_args()
    raw=args.diagnostic_receipt.read_bytes();assert hashlib.sha256(raw).hexdigest()=='aba3e32beef66ec5ca6919e65f5e49f8b40f587599a8d2f43205e60bab873701'
    old=json.loads(raw);assert old['GITHUB_RUN_ID']=='41135044960' and old['GITHUB_SHA']=='f0a8364afd3380eb0e25b8d82824dd1752f91e16'
    source_policy()
    tests(old)
