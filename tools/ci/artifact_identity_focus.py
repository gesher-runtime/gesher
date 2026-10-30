"""Focused metadata-only replay; prior producer context is never global GITHUB context."""
import ast
import copy
import datetime
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
from receipt_identity import identity_record, job_snapshot, exact_run, run_snapshot

BASE = '00f96217616c5892dd8364067d007f92d541585b'


def original_python(tree):
    for node in tree.body:
        if isinstance(node, ast.If) and any(isinstance(x, ast.Constant) and x.value ==
                'Producer source/attempt/conclusion mismatch' for x in ast.walk(node)):
            predicate = node.test
    selection = next(node.value for node in tree.body if isinstance(node, ast.Assign)
                     and any(isinstance(t, ast.Name) and t.id == 'rows' for t in node.targets)
                     and isinstance(node.value, ast.ListComp)
                     and 'jobs' in ast.dump(node.value.generators[0].iter))
    return selection, predicate


def reject_python(job_rows, spec, expected, selection, predicate):
    scope = {'jobs': {'jobs': job_rows}, 'a': SimpleNamespace(producer=spec['requested_producer'],
             producer_suffix=spec['suffix'])}
    rows = eval(compile(ast.Expression(selection), 'original-selection', 'eval'), {}, scope)
    identity_record('producer', {**expected, 'producer': spec['requested_producer'],
                    'suffix': spec['suffix'], 'required_count': 1, 'allowed_conclusions': ['success']},
                    [job_snapshot(row) for row in rows])
    if len(rows) != 1:
        return True
    scope.update(job=rows[0], allowed={'success'}, head=expected['source'],
                 run_id=expected['run_id'], attempt=expected['attempt'])
    try:
        return bool(eval(compile(ast.Expression(predicate), 'original-predicate', 'eval'), {}, scope))
    except KeyError:
        return True  # The original missing-field exception fails closed.


def mutations(row):
    for field, value in (('head_sha', 'f'*40), ('run_id', 1), ('run_attempt', 2),
                         ('name', 'wrong-role'), ('conclusion', 'failure'), ('head_sha', None)):
        changed = copy.deepcopy(row); changed[field] = value
        yield field, changed
    changed = copy.deepcopy(row); changed.update(status='in_progress', conclusion=None)
    yield 'nonterminal-observation', changed
    for field in ('head_sha','run_id','run_attempt'):
        changed=copy.deepcopy(row); del changed[field]
        yield 'missing:'+field, changed


def powershell_cases(ps_rows, expected):
    paths=['tools/windows4/run-tsf-union.ps1','tools/windows4/run-tsf-component.ps1',
           '.github/workflows/native3-locks-blocking.yml','.github/workflows/native3-locks-commands.yml']
    cases=[]
    for path in paths:
        before=subprocess.check_output(['git','show',BASE+':'+path],text=True)
        after=Path(path).read_text()
        if path.endswith('.yml'):
            def block(text):
                text=text.split('      - name: Verify exact successful producer, source and artifact before execution\n',1)[1]
                lines=text.split('        run: |\n',1)[1].splitlines()
                result=[]
                for line in lines:
                    if line and not line.startswith('          '): break
                    result.append(line[10:] if line else '')
                return '\n'.join(result)
            before,after=block(before),block(after)
        selected=next(row for row in ps_rows if row['spec']['client']==('tsf' if 'tsf-' in path else 'native'))
        job=copy.deepcopy(selected['job']); producer=selected['spec']['producer']
        synthetic=path.endswith('run-tsf-component.ps1') or path.endswith('native3-locks-commands.yml')
        if synthetic: job['name']=producer='compile'
        cases.append({'path':path,'before':before,'after':after,'job':job,
                      'expected':{**expected,'producer':producer,'synthetic_role_projection':synthetic},
                      'synthetic_role_projection':synthetic})
    return cases


def main():
    if os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('Actual receipt metadata replay is CI only')
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    assert head == os.environ['GITHUB_SHA']
    descriptor = json.loads(Path('tools/ci/artifact-identity-suppliers.json').read_text())
    assert descriptor['source'] == BASE and descriptor['run'] == '41064959428' and descriptor['attempt'] == '1'
    repo = descriptor['repository']
    def api(path): return json.loads(subprocess.check_output(['gh', 'api', f'repos/{repo}/{path}']))
    run = api('actions/runs/'+descriptor['run'])
    assert exact_run(run, descriptor['run'], descriptor['attempt'], BASE,
                     descriptor['repository_id'], descriptor['repository_id'], repo, 'ci')
    identity_record('prior_run', descriptor, run_snapshot(run))
    jobs = api(f"actions/runs/{descriptor['run']}/attempts/1/jobs?per_page=100")
    artifacts = api(f"actions/runs/{descriptor['run']}/artifacts?per_page=100")
    assert jobs['total_count'] == len(jobs['jobs']) <= 100
    assert artifacts['total_count'] == len(artifacts['artifacts']) <= 100
    print('RECEIPT_VERIFIER_CONTEXT '+json.dumps({'verifier_source':head,
          'verifier_run':os.environ['GITHUB_RUN_ID'],'verifier_attempt':os.environ['GITHUB_RUN_ATTEMPT'],
          'supplier_source':BASE,'supplier_run':descriptor['run'],'supplier_attempt':'1',
          'scope':'metadata-only; no archive/runtime qualification; historical cause unknown'}),flush=True)
    path='tools/ci/artifact_receipt.py'
    before=ast.parse(subprocess.check_output(['git','show',BASE+':'+path],text=True))
    after=ast.parse(Path(path).read_text())
    conditions=lambda tree:[ast.dump(n.test,include_attributes=False) for n in ast.walk(tree) if isinstance(n,ast.If)]
    assert conditions(before)==conditions(after), 'Original Python acceptance predicates changed'
    helper='tools/ci/receipt_identity.py'
    original_helper=ast.parse(subprocess.check_output(['git','show',BASE+':'+helper],text=True))
    current_helper=ast.parse(Path(helper).read_text())
    for name in ('exact_run','failed_installer_allowed','installer_uploaded'):
        old_function=next(n for n in original_helper.body if isinstance(n,ast.FunctionDef) and n.name==name)
        new_function=next(n for n in current_helper.body if isinstance(n,ast.FunctionDef) and n.name==name)
        assert ast.dump(old_function)==ast.dump(new_function), ('Original helper policy changed',name)
    old=original_python(before); new=original_python(after)
    assert all(ast.dump(a)==ast.dump(b) for a,b in zip(old,new))
    expected={'run_id':descriptor['run'],'attempt':'1','source':BASE,'repository':repo,
              'repository_id':descriptor['repository_id']}
    ps_rows=[]
    for spec in descriptor['suppliers']:
        rows=[row for row in jobs['jobs'] if row['id']==spec['producer_id']]
        assert len(rows)==1
        job=rows[0]
        assert job['name']==spec['producer'] and str(job['run_id'])==descriptor['run']
        assert str(job['run_attempt'])=='1' and job['head_sha']==BASE and job['conclusion']=='success'
        items=[item for item in artifacts['artifacts'] if item['id']==spec['artifact']['id']]
        assert len(items)==1
        item=items[0]
        assert all(item[key]==value for key,value in spec['artifact'].items()) and not item['expired']
        assert item['workflow_run']['id']==int(descriptor['run']) and item['workflow_run']['head_sha']==BASE
        assert str(item['workflow_run']['repository_id'])==descriptor['repository_id']
        assert str(item['workflow_run']['head_repository_id'])==descriptor['repository_id']
        timestamp=lambda value:datetime.datetime.fromisoformat(value.replace('Z','+00:00'))
        assert timestamp(job['started_at'])<=timestamp(item['created_at'])<=timestamp(job['completed_at'])
        identity_record('prior_supplier_artifact',spec, {key:item.get(key) for key in
                        ('id','name','digest','expired','created_at','workflow_run')})
        if spec['client']=='python':
            assert not reject_python([job],spec,{**expected,'diagnostic_case':'actual_prior_run'},*new)
            for field,changed in mutations(job):
                assert reject_python([changed],spec,{**expected,'diagnostic_case':'counterfactual:'+field},*new), (spec['role'],field)
            assert reject_python([job,job],spec,{**expected,'diagnostic_case':'counterfactual:duplicate'},*new)
            print('RECEIPT_ORIGINAL_PREDICATE_PASS '+spec['role']+' wrong/missing source/run/attempt/role/conclusion/nonterminal/duplicate rejected',flush=True)
        else:
            ps_rows.append({'spec':spec,'job':job})
    cases=powershell_cases(ps_rows,expected)
    with tempfile.TemporaryDirectory(prefix='receipt-replay-') as temp:
        fixture=Path(temp)/'cases.json'; fixture.write_text(json.dumps(cases),encoding='utf-8')
        subprocess.run(['pwsh','-NoProfile','-File','tools/ci/test-receipt-diagnostics.ps1',
                        '-FixturePath',str(fixture)],check=True)
    assert not subprocess.check_output(['git','status','--porcelain','--untracked-files=all'],text=True).strip()
    print('ARTIFACT_IDENTITY_DIAGNOSTIC_PASS historical_cause=UNKNOWN acceptance_predicates=UNCHANGED archives=NOT_DOWNLOADED runtime_qualification=false',flush=True)


if __name__=='__main__': main()
