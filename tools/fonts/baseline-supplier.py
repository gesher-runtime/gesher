"""CI-only explicit runtime and framework comparison suppliers, never fixture receipts."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'ci'))
from receipt_identity import identity_record, job_snapshot, run_snapshot
from font_records import emit

REPO = 'gesher-runtime/gesher'
REPO_ID = '1426153186'
RUN = 40953048849
SOURCE = '42076a8f28c0d23eee5b10ee0acf0844be0dc277'
SUPPLIERS = {
    'old-runtime': {'artifact': 'windows-x64', 'id': 12934605958,
                'digest': 'sha256:64089fbce38e36da3ab72823837aaa3c7b78ca500d53e675b899c1091948c6a9',
                'producer': 'build', 'job': 123726050564},
    'old-framework': {'artifact': 'framework', 'id': 12932739219,
                  'digest': 'sha256:a66c2c9546825e8fdca40fc4cde4156419bc4dae60fe2acab29ff160e617d3ef',
                  'producer': 'framework / framework', 'job': 123733913285},
    'qualified-data': {'artifact':'windows-x64','id':12990368305,
        'digest':'sha256:27d03cec3d032263e467da92e725ca1a20c1a26dcc7f5f4eda0ead9c522cbacd',
        'producer':'build','job':124142855542,'run':41078972594,
        'source':'1bf8fad9ec3b423ddef8c97c1d72b25790974364','overall_conclusion':'success'},
    'qualified-translator': {'artifact':'arm64','id':12988403859,
        'digest':'sha256:d4acec0555947e3097cffd445e5d244d1c5ff032e02b4893c580e6e4645259f8',
        'producer':'arm64 / build','job':124143790496,'run':41078972594,
        'source':'1bf8fad9ec3b423ddef8c97c1d72b25790974364','overall_conclusion':'success'},
    'current-runtime': {'artifact': 'windows-x64', 'id': 12980654577,
        'digest': 'sha256:1a33a935e1758802df1eb1b636968854d6125e63ec807e8f19f62e80ba43adf1',
        'producer': 'build', 'job': 124093862352,
        'run': 41064959428, 'source': '00f96217616c5892dd8364067d007f92d541585b',
        'overall_conclusion': 'failure'},
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def validate(run, jobs, artifacts, role):
    expected = SUPPLIERS[role]
    wanted_run=expected.get('run',RUN); wanted_source=expected.get('source',SOURCE)
    identity_record('font_supplier_run', {'role':role,'run':wanted_run,'source':wanted_source,'attempt':1,'repository':REPO,'repository_id':REPO_ID,'conclusion':expected.get('overall_conclusion','success')}, run_snapshot(run))
    identity_record('font_supplier_inventory', {'role':role,'producer':expected['producer'],'producer_id':expected['job'],'artifact':expected['artifact'],'artifact_id':expected['id']}, {'jobs_total':jobs.get('total_count'),'jobs_received':len(jobs.get('jobs',[])),'artifacts_total':artifacts.get('total_count'),'artifact_names':[x.get('name') for x in artifacts.get('artifacts',[])]})
    identity_record('font_supplier_producer', {'role':role,'source':wanted_source,'run':wanted_run,'attempt':1,'id':expected['job'],'name':expected['producer'],'conclusion':'success'}, [job_snapshot(x) for x in jobs.get('jobs',[]) if x.get('id')==expected['job'] or x.get('name')==expected['producer']])
    identity_record('font_supplier_artifact', {'role':role,'id':expected['id'],'name':expected['artifact'],'digest':expected['digest'],'source':wanted_source,'run':wanted_run,'repository_id':REPO_ID}, [{k:x.get(k) for k in ('id','name','digest','expired','size_in_bytes','created_at','workflow_run')} for x in artifacts.get('artifacts',[]) if x.get('id')==expected['id'] or x.get('name')==expected['artifact']])
    require(run['id'] == wanted_run and run['run_attempt'] == 1 and run['head_sha'] == wanted_source
            and run['status'] == 'completed' and run['conclusion'] == expected.get('overall_conclusion','success')
            and run['name'] == 'ci', 'Comparison supplier run/source/attempt/conclusion differs')
    require(run['repository']['full_name'] == REPO
            and str(run['repository']['id']) == REPO_ID
            and str(run['head_repository']['id']) == REPO_ID, 'Comparison supplier repository differs')
    require(jobs['total_count'] <= 100 and artifacts['total_count'] <= 100
            and len(jobs['jobs']) == jobs['total_count']
            and len(artifacts['artifacts']) == artifacts['total_count'], 'Incomplete baseline inventory')
    selected = [job for job in jobs['jobs'] if job['id'] == expected['job'] and job['name'] == expected['producer']]
    require(len(selected) == 1, 'Missing/ambiguous comparison producer')
    job = selected[0]
    require(job['head_sha'] == wanted_source and job['run_id'] == wanted_run and job['run_attempt'] == 1
            and job['conclusion'] == 'success' and job['status'] == 'completed', 'Comparison producer identity differs')
    selected = [item for item in artifacts['artifacts'] if item['name'] == expected['artifact']]
    require(len(selected) == 1, 'Missing/ambiguous comparison artifact role')
    item = selected[0]
    require(item['id'] == expected['id'] and item['digest'] == expected['digest']
            and not item['expired'] and item['size_in_bytes'] > 0, 'Comparison artifact identity/digest differs')
    identity = item['workflow_run']
    require(identity['id'] == wanted_run and identity['head_sha'] == wanted_source
            and str(identity['repository_id']) == REPO_ID
            and str(identity['head_repository_id']) == REPO_ID, 'Comparison artifact source/repository differs')
    time = lambda value: datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
    require(time(job['started_at']) <= time(item['created_at']) <= time(job['completed_at']),
            'Comparison artifact upload outside exact producer attempt')
    return job, item


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('role', choices=SUPPLIERS)
    parser.add_argument('--destination', required=True, type=Path)
    args = parser.parse_args()
    require(os.environ.get('GITHUB_ACTIONS') == 'true' and os.environ['GITHUB_REPOSITORY'] == REPO,
            'Baseline archives may be downloaded only within the reviewed CI')
    require(not args.destination.exists(), 'Baseline destination must be fresh')
    def api(path):
        return json.loads(subprocess.check_output(['gh', 'api', f'repos/{REPO}/{path}']))
    spec=SUPPLIERS[args.role]; wanted_run=spec.get('run',RUN)
    wanted_source=spec.get('source',SOURCE)
    job, item = validate(api(f'actions/runs/{wanted_run}'),
                         api(f'actions/runs/{wanted_run}/attempts/1/jobs?per_page=100'),
                         api(f'actions/runs/{wanted_run}/artifacts?per_page=100'), args.role)
    subprocess.run([sys.executable, 'tools/surface/download-verified-artifact.py', '--id', str(item['id']),
                    '--digest', item['digest'], '--destination', str(args.destination)], check=True)
    members={}
    for path in args.destination.rglob('*'):
        require(not path.is_symlink(), 'Linked extracted supplier')
        if path.is_file():members[path.relative_to(args.destination).as_posix()]=hashlib.sha256(path.read_bytes()).hexdigest()
    require(bool(members), 'Empty extracted comparison supplier')
    receipt={'role':args.role,'run':wanted_run,'attempt':1,'source':wanted_source,'repository':REPO,
        'repository_id':REPO_ID,'producer':job_snapshot(job),'artifact':item,
        'current_fixture_supplier':False,'members':members}
    target=args.destination.parent/(args.destination.name+'.receipt.json')
    require(not target.exists(), 'Supplier receipt path already exists')
    target.write_text(json.dumps(receipt,sort_keys=True)+'\n',encoding='utf-8')
    emit('FONT_COMPARISON_SUPPLIER_METADATA',{key:value for key,value in receipt.items() if key!='members'})
    for name,digest in members.items():emit('FONT_COMPARISON_SUPPLIER_MEMBER',{'role':args.role,'path':name,'sha256':digest})
    emit('FONT_COMPARISON_SUPPLIER_RECEIPT',{'role':args.role,'path':str(target),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'members':len(members)})


if __name__ == '__main__':
    main()
