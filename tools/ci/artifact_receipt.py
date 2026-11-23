"""CI-only current-run supplier receipt and fail-closed archive verification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from artifact_candidates import read_artifacts, select_artifact
from github_read import get_json
from github_jobs import get_jobs
from receipt_identity import (exact_run, failed_installer_allowed, installer_uploaded,
                              identity_record, job_snapshot, run_snapshot)

ROOT = Path(__file__).resolve().parents[2]


def context():
    if os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('CI only')
    value = {key: os.environ[name] for key, name in (
        ('repo', 'GITHUB_REPOSITORY'), ('run_id', 'GITHUB_RUN_ID'),
        ('attempt', 'GITHUB_RUN_ATTEMPT'), ('repo_id', 'GITHUB_REPOSITORY_ID'),
        ('checkout', 'GITHUB_SHA'), ('workflow', 'GITHUB_WORKFLOW'))}
    check_checkout(value)
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8'))
    value['head'], value['head_repo'] = value['checkout'], value['repo_id']
    if os.environ['GITHUB_EVENT_NAME'] == 'pull_request':
        value['head'] = event['pull_request']['head']['sha']
        value['head_repo'] = str(event['pull_request']['head']['repo']['id'])
    return value


def check_checkout(value):
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() != value['checkout']:
        raise RuntimeError('Checkout mismatch')


def check_run(value):
    run = get_json(f"repos/{value['repo']}/actions/runs/{value['run_id']}")
    identity_record('run', {'run_id': value['run_id'], 'attempt': value['attempt'],
        'source': value['head'], 'checkout': value['checkout'], 'repository': value['repo'],
        'repository_id': value['repo_id'], 'head_repository_id': value['head_repo'],
        'workflow': value['workflow']}, run_snapshot(run), 'observed')
    if not exact_run(run, value['run_id'], value['attempt'], value['head'], value['repo_id'],
                     value['head_repo'], value['repo'], value['workflow']):
        raise RuntimeError('Current run/source/attempt/repository mismatch')


def read_jobs(value):
    jobs = get_jobs(f"repos/{value['repo']}/actions/runs/{value['run_id']}/attempts/{value['attempt']}/jobs")
    if jobs['total_count'] > 100:
        raise RuntimeError('Incomplete job inventory')
    return jobs


def select_producer(value, jobs, name, suffix=False, allow_failed=False):
    rows = [x for x in jobs['jobs'] if x['name'] == name or (suffix and x['name'].endswith(' / ' + name))]
    allowed = {'success', 'failure'} if allow_failed else {'success'}
    identity_record('producer', {'run_id': value['run_id'], 'attempt': value['attempt'],
        'source': value['head'], 'checkout': value['checkout'], 'producer': name, 'suffix': suffix,
        'workflow': value['workflow'], 'allowed_conclusions': sorted(allowed), 'required_count': 1,
        'status_policy': 'observed only; original predicate unchanged'},
        {'total_count': jobs.get('total_count'), 'received_count': len(jobs['jobs']),
         'selected': [job_snapshot(row) for row in rows],
         'observed_names': [row.get('name') for row in jobs['jobs']]})
    if len(rows) != 1:
        raise RuntimeError('Producer job missing or ambiguous')
    job = rows[0]
    if (job['status'] != 'completed' or job['conclusion'] not in allowed or job['head_sha'] != value['head']
            or str(job['run_id']) != value['run_id'] or str(job['run_attempt']) != value['attempt']):
        raise RuntimeError('Producer source/attempt/conclusion mismatch')
    if allow_failed and not installer_uploaded(job):
        raise RuntimeError('Installer upload did not complete successfully')
    return job


def select_supplier(value, inventory, name, job, *, total_count_max=100):
    identity_record('artifact_inventory', {'name': name, 'run_id': value['run_id'], 'source': value['head'],
        'repository_id': value['repo_id'], 'head_repository_id': value['head_repo'],
        'total_count_max': total_count_max},
        {'total_count': len(inventory), 'received_count': len(inventory),
         'names': [item['name'] for item in inventory]}, 'observed')
    item = select_artifact(inventory, name, job, run_id=value['run_id'], repo_id=value['repo_id'],
                           head_repo_id=value['head_repo'], head=value['head'])
    identity_record('artifact', {'name': name, 'run_id': value['run_id'], 'attempt': value['attempt'],
        'source': value['head'], 'repository_id': value['repo_id'], 'head_repository_id': value['head_repo'],
        'expired': False, 'digest_pattern': 'sha256:[0-9a-f]{64}', 'size_min': 1,
        'producer_id': job['id'], 'upload_not_before': job['started_at'],
        'upload_not_after': job['completed_at']},
        {**{key: item.get(key) for key in ('id', 'name', 'digest', 'expired', 'size_in_bytes', 'created_at')},
         'workflow_run': {key: (item.get('workflow_run') or {}).get(key) for key in
                          ('id', 'head_sha', 'repository_id', 'head_repository_id')}}, 'observed')
    return item


def download(item, destination):
    subprocess.run([sys.executable, 'tools/surface/download-verified-artifact.py',
                    '--id', str(item['id']), '--digest', item['digest'],
                    '--destination', str(destination)], check=True, stdout=sys.stdout)


def receipt(value, item, job):
    print(f"CI_ARTIFACT_RECEIPT name={item['name']} id={item['id']} digest={item['digest']} "
          f"producer={job['name']} producer_id={job['id']} conclusion={job['conclusion']} "
          f"source={value['head']} checkout={value['checkout']} run={value['run_id']} "
          f"attempt={value['attempt']}", flush=True)


def single(args):
    if args.allow_failed_producer and not failed_installer_allowed(
            os.environ.get('GITHUB_WORKFLOW'), os.environ.get('GITHUB_JOB'),
            args.producer, args.name, args.producer_suffix):
        raise RuntimeError('Failed producer exception is restricted to canonical e2e installer')
    value = context()
    check_run(value)
    job = select_producer(value, read_jobs(value), args.producer,
                          args.producer_suffix, args.allow_failed_producer)
    inventory = read_artifacts(f"repos/{value['repo']}/actions/runs/{value['run_id']}/artifacts", name=args.name)
    item = select_supplier(value, inventory, args.name, job)
    download(item, args.destination)
    receipt(value, item, job)


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('Duplicate batch manifest key')
        value[key] = item
    return value


def _invalid_constant(_):
    raise ValueError('Invalid JSON constant')


def batch_manifest(raw):
    specs = json.loads(raw, object_pairs_hook=_unique_object, parse_constant=_invalid_constant)
    if not isinstance(specs, list) or not 1 <= len(specs) <= 100:
        raise ValueError('Invalid batch manifest size')
    names, destinations = set(), set()
    for spec in specs:
        if not isinstance(spec, dict) or set(spec) != {'name', 'producer', 'destination'}:
            raise ValueError('Invalid batch manifest fields')
        if not all(isinstance(text, str) and text and text == text.strip()
                   and len(text) <= 200 and not re.search(r'[\x00-\x1f\x7f]', text)
                   for text in spec.values()):
            raise ValueError('Invalid batch manifest value')
        destination = spec['destination']
        if (not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}', destination)
                or destination.endswith('.')
                or re.fullmatch(r'(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?', destination)):
            raise ValueError('Unsafe batch destination')
        if spec['name'].casefold() in names or destination.casefold() in destinations:
            raise ValueError('Duplicate batch artifact or destination')
        names.add(spec['name'].casefold())
        destinations.add(destination.casefold())
    return specs


def batch_snapshot(value, specs, *, revalidate=False):
    if not revalidate:
        check_run(value)
    jobs = read_jobs(value)
    producers = {spec['name']: select_producer(value, jobs, spec['producer']) for spec in specs}
    # The existing bounded selector reads all pages and checks totals/unique IDs.
    inventory = read_artifacts(f"repos/{value['repo']}/actions/runs/{value['run_id']}/artifacts")
    selected = {spec['name']: (producers[spec['name']], select_supplier(
        value, inventory, spec['name'], producers[spec['name']], total_count_max=500)) for spec in specs}
    if revalidate:
        # This is the final API read before publish, so a new attempt that
        # starts during the job/artifact inventories still invalidates the batch.
        check_run(value)
    return selected


def supplier_identity(selected):
    job, item = selected
    return ({key: job.get(key) for key in ('id', 'name', 'status', 'conclusion', 'head_sha',
             'run_id', 'run_attempt', 'started_at', 'completed_at')},
            {key: item.get(key) for key in ('id', 'name', 'digest', 'expired', 'size_in_bytes',
             'created_at', 'workflow_run')})


def _linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def new_destination(path):
    destination = Path(path).absolute()
    try:
        destination.resolve().relative_to(ROOT.resolve())
    except ValueError:
        raise RuntimeError('Batch destination is outside the checked-out repository') from None
    for parent in (destination, *destination.parents):
        if _linked(parent):
            raise RuntimeError('Batch destination has a linked path component')
    if os.path.lexists(destination):
        raise RuntimeError('Batch destination already exists')
    return destination


def committed_manifest(path, raw_manifest, value):
    manifest = Path(path).resolve()
    try:
        relative = manifest.relative_to(ROOT.resolve()).as_posix()
    except ValueError:
        raise RuntimeError('Batch manifest is outside the checked-out repository') from None
    committed = subprocess.check_output(['git', 'show', f"{value['checkout']}:{relative}"], cwd=ROOT)
    # Git can check text out with CRLF on Windows. Only that line-ending
    # conversion is permitted; the bytes parsed above remain bound to the blob.
    if raw_manifest.replace(b'\r\n', b'\n') != committed.replace(b'\r\n', b'\n'):
        raise RuntimeError('Batch manifest differs from the exact checkout')
    return relative, committed


def batch(args):
    raw_manifest = Path(args.batch).read_bytes()
    specs = batch_manifest(raw_manifest)
    destination = new_destination(args.destination)
    value = context()
    relative, committed = committed_manifest(args.batch, raw_manifest, value)
    selected = batch_snapshot(value, specs)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=destination.name + '-stage-', dir=destination.parent) as temporary:
        staging = Path(temporary)
        for spec in specs:
            download(selected[spec['name']][1], staging / spec['destination'])
        # No staged bytes are visible to assembly. Re-read live evidence after
        # every download and reject changed selected suppliers or run attempts.
        current = batch_snapshot(value, specs, revalidate=True)
        if any(supplier_identity(selected[name]) != supplier_identity(current[name]) for name in selected):
            raise RuntimeError('Batch supplier identity changed before publication')
        check_checkout(value)
        # Reserve a new container exclusively: never replace a prior batch or an
        # existing/symlinked destination. Publish all its artifacts in one rename.
        destination.mkdir(mode=0o700)
        try:
            os.rename(staging, destination / 'artifacts')
        except BaseException:
            destination.rmdir()
            raise
    for spec in specs:
        job, item = current[spec['name']]
        receipt(value, item, job)
    print(f'CI_ARTIFACT_BATCH version=1 count={len(specs)} manifest_sha256={hashlib.sha256(committed).hexdigest()} '
          f'source={value["head"]} checkout={value["checkout"]} run={value["run_id"]} '
          f'attempt={value["attempt"]} destination={destination / "artifacts"}', flush=True)
    # Other callers (component_producer.py) pass a namespace without a ledger flag.
    if getattr(args, 'ledger', False):
        write_ledger(destination, value, specs, relative, committed, current)
    return current


# A supplier bundle carries one verified batch to many consumers of the same
# run: the batch is received once, with every check above, and its published
# tree is uploaded whole as one artifact. Each consumer then receipts that one
# artifact from its exact producer job and checks the ledger below against the
# same committed manifest, the current run/source and every extracted byte.
LEDGER = 'receipts.json'
_LEDGER_LIMIT = 16 * 1024 * 1024
_LEDGER_KEYS = {'version', 'manifest', 'manifest_sha256', 'repository', 'repository_id',
                'head_repository_id', 'workflow', 'run_id', 'attempt', 'source', 'checkout',
                'suppliers', 'files'}
_SHA256 = re.compile(r'[0-9a-f]{64}')


def tree_digests(root):
    """SHA-256 of every regular file below root; links and other types are refused."""
    files = {}
    for path in sorted(Path(root).rglob('*')):
        if _linked(path):
            raise RuntimeError('Supplier bundle contains a link')
        if path.is_dir():
            continue
        if not path.is_file():
            raise RuntimeError('Supplier bundle contains a non-regular file')
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
        files[path.relative_to(root).as_posix()] = digest.hexdigest()
    return files


def write_ledger(destination, value, specs, relative, committed, current):
    suppliers = []
    for spec in specs:
        job, item = current[spec['name']]
        suppliers.append({**spec,
            'artifact': {key: item[key] for key in ('id', 'digest', 'size_in_bytes', 'created_at')},
            'producer_job': {key: job[key] for key in ('id', 'name', 'conclusion', 'started_at', 'completed_at')}})
    files = {'artifacts/' + name: digest for name, digest in tree_digests(destination / 'artifacts').items()}
    ledger = {'version': 1, 'manifest': relative, 'manifest_sha256': hashlib.sha256(committed).hexdigest(),
              'repository': value['repo'], 'repository_id': value['repo_id'],
              'head_repository_id': value['head_repo'], 'workflow': value['workflow'],
              'run_id': value['run_id'], 'attempt': value['attempt'], 'source': value['head'],
              'checkout': value['checkout'], 'suppliers': suppliers, 'files': files}
    # Exclusive creation: a ledger is written once, beside the published batch.
    with (destination / LEDGER).open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(ledger, stream, ensure_ascii=True, sort_keys=True, indent=1)
    print(f'CI_ARTIFACT_LEDGER version=1 count={len(specs)} files={len(files)} '
          f'manifest_sha256={ledger["manifest_sha256"]} run={value["run_id"]} attempt={value["attempt"]}',
          flush=True)


def _positive(value):
    return type(value) is int and value > 0


def verify_bundle(staging, specs, relative, committed, value):
    """Accept an extracted bundle only if it is exactly the ledgered batch."""
    if {entry.name for entry in staging.iterdir()} != {'artifacts', LEDGER}:
        raise RuntimeError('Supplier bundle layout differs from a published batch')
    ledger_path = staging / LEDGER
    if _linked(ledger_path) or not ledger_path.is_file() or ledger_path.stat().st_size > _LEDGER_LIMIT:
        raise RuntimeError('Supplier bundle ledger is not a bounded regular file')
    try:
        ledger = json.loads(ledger_path.read_bytes(), object_pairs_hook=_unique_object,
                            parse_constant=_invalid_constant)
    except ValueError as error:
        raise RuntimeError(f'Supplier bundle ledger is not strict JSON: {error}') from None
    if (not isinstance(ledger, dict) or set(ledger) != _LEDGER_KEYS
            or type(ledger['version']) is not int or ledger['version'] != 1):
        raise RuntimeError('Supplier bundle ledger has invalid fields')
    expected = {'manifest': relative, 'manifest_sha256': hashlib.sha256(committed).hexdigest(),
                'repository': value['repo'], 'repository_id': value['repo_id'],
                'head_repository_id': value['head_repo'], 'workflow': value['workflow'],
                'run_id': value['run_id'], 'source': value['head'], 'checkout': value['checkout']}
    if any(ledger[key] != item for key, item in expected.items()):
        raise RuntimeError('Supplier bundle ledger is bound to another manifest, run or source')
    # A partial rerun can reuse the bundle job of an earlier attempt of this
    # run, exactly as it reuses that attempt's producers; never a later one.
    attempt = ledger['attempt']
    if (not isinstance(attempt, str) or not re.fullmatch(r'[1-9][0-9]{0,9}', attempt)
            or int(attempt) > int(value['attempt'])):
        raise RuntimeError('Supplier bundle ledger attempt is invalid')
    suppliers = ledger['suppliers']
    if not isinstance(suppliers, list) or len(suppliers) != len(specs):
        raise RuntimeError('Supplier bundle ledger does not match the manifest')
    for spec, entry in zip(specs, suppliers):
        if (not isinstance(entry, dict) or set(entry) != {*spec, 'artifact', 'producer_job'}
                or any(entry[key] != spec[key] for key in spec)):
            raise RuntimeError('Supplier bundle ledger does not match the manifest')
        artifact, job = entry['artifact'], entry['producer_job']
        if (not isinstance(artifact, dict) or set(artifact) != {'id', 'digest', 'size_in_bytes', 'created_at'}
                or not _positive(artifact['id']) or not _positive(artifact['size_in_bytes'])
                or not isinstance(artifact['digest'], str)
                or not re.fullmatch(r'sha256:[0-9a-f]{64}', artifact['digest'])
                or not isinstance(job, dict)
                or set(job) != {'id', 'name', 'conclusion', 'started_at', 'completed_at'}
                or not _positive(job['id']) or job['name'] != spec['producer']
                or job['conclusion'] != 'success'):
            raise RuntimeError('Supplier bundle ledger has an unverified supplier')
    artifacts = staging / 'artifacts'
    if _linked(artifacts) or not artifacts.is_dir():
        raise RuntimeError('Supplier bundle layout differs from a published batch')
    owners = list(artifacts.iterdir())
    if (any(_linked(owner) or not owner.is_dir() for owner in owners)
            or sorted(owner.name for owner in owners) != sorted(spec['destination'] for spec in specs)):
        raise RuntimeError('Supplier bundle members differ from the manifest')
    files = ledger['files']
    if (not isinstance(files, dict)
            or not all(isinstance(digest, str) and _SHA256.fullmatch(digest) for digest in files.values())):
        raise RuntimeError('Supplier bundle ledger has invalid file digests')
    actual = {'artifacts/' + name: digest for name, digest in tree_digests(artifacts).items()}
    if actual != files:
        raise RuntimeError('Supplier bundle files differ from the verified batch')
    return ledger


def bundle(args):
    raw_manifest = Path(args.bundle_of).read_bytes()
    specs = batch_manifest(raw_manifest)
    destination = new_destination(args.destination)
    value = context()
    relative, committed = committed_manifest(args.bundle_of, raw_manifest, value)
    check_run(value)
    job = select_producer(value, read_jobs(value), args.producer)
    endpoint = f"repos/{value['repo']}/actions/runs/{value['run_id']}/artifacts"
    item = select_supplier(value, read_artifacts(endpoint, name=args.name), args.name, job)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=destination.name + '-stage-', dir=destination.parent) as temporary:
        staging = Path(temporary) / 'bundle'
        download(item, staging)
        ledger = verify_bundle(staging, specs, relative, committed, value)
        # As for a batch, re-read live evidence after the download: the bundle
        # producer, its artifact and the current run attempt must be unchanged.
        current_job = select_producer(value, read_jobs(value), args.producer)
        current_item = select_supplier(value, read_artifacts(endpoint, name=args.name), args.name, current_job)
        check_run(value)
        if supplier_identity((job, item)) != supplier_identity((current_job, current_item)):
            raise RuntimeError('Supplier bundle identity changed before publication')
        check_checkout(value)
        destination.mkdir(mode=0o700)
        try:
            os.rename(staging / 'artifacts', destination / 'artifacts')
        except BaseException:
            destination.rmdir()
            raise
    for entry in ledger['suppliers']:
        print(f"CI_ARTIFACT_BUNDLE_MEMBER name={entry['name']} id={entry['artifact']['id']} "
              f"digest={entry['artifact']['digest']} producer={entry['producer_job']['name']} "
              f"producer_id={entry['producer_job']['id']} source={value['head']} run={value['run_id']} "
              f"attempt={ledger['attempt']} bundle={item['id']}", flush=True)
    receipt(value, item, current_job)
    print(f"CI_ARTIFACT_BUNDLE version=1 count={len(specs)} files={len(ledger['files'])} "
          f"manifest_sha256={ledger['manifest_sha256']} bundle={item['id']} digest={item['digest']} "
          f"source={value['head']} checkout={value['checkout']} run={value['run_id']} "
          f"attempt={value['attempt']} destination={destination / 'artifacts'}", flush=True)
    return ledger


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--name')
    mode.add_argument('--batch', help='strict committed JSON supplier manifest')
    parser.add_argument('--producer')
    parser.add_argument('--producer-suffix', action='store_true')
    parser.add_argument('--destination', required=True)
    parser.add_argument('--allow-failed-producer', action='store_true')
    parser.add_argument('--ledger', action='store_true',
                        help='also write a supplier bundle ledger beside the published batch')
    parser.add_argument('--bundle-of', metavar='MANIFEST',
                        help='receive --name as a supplier bundle of this committed batch manifest')
    args = parser.parse_args(argv)
    if args.batch:
        if (args.producer is not None or args.producer_suffix or args.allow_failed_producer
                or args.bundle_of is not None):
            parser.error('Batch suppliers require exact successful producers')
        batch(args)
    else:
        if not args.producer:
            parser.error('--producer is required for a single artifact')
        if args.ledger:
            parser.error('--ledger requires --batch')
        if args.bundle_of is not None:
            if args.producer_suffix or args.allow_failed_producer:
                parser.error('A supplier bundle requires its exact successful producer')
            bundle(args)
        else:
            single(args)


if __name__ == '__main__':
    main()
