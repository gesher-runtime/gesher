"""CI-only fail-closed assembly of individually verified Surface artifacts."""
import argparse
import json
import os
from pathlib import Path
import shutil

FRAMEWORK = set('framework.jar framework-res.apk gesher-app.jar build.prop lifecycle.apk components.apk peer.apk uiapp.apk dbapp.apk devsvc.apk devices.apk nodevices.apk popupapp.apk gesher-camera.apk rtlapp.apk imeapp.apk lifeapp.apk localeapp.apk locale2app.apk locale2other.apk surfaceapp.apk winapp.apk winlegacy.apk netapp.apk nonetapp.apk web3app.apk services-tests.dex exitstatusapp.apk sharedlibraryapp.apk sharedlibrarydlc.apk sharedlibrarybad.apk shared-base-v1.jar shared-base-v2.jar shared-entry.jar graphics-tests.dex web3-query-tests.dex'.split())
HOST = set('dalvikvm.exe dex2oat.exe profman.exe dexdump.exe gesher-exec.exe arm64_elf_test.exe surface_invalidation_test.exe dalvikvm.map dex2oat.map core-oj.jar core-libart.jar core-icu4j.jar hello.dex'.split())
ALLOWED = {'current_framework': ('base_framework', FRAMEWORK), 'current_host': ('base_windows_x64', HOST)}

def assemble(source, dest, ledger, owner, prefix='', exclude=()):
    source, dest, ledger = Path(source).resolve(), Path(dest).resolve(), Path(ledger).resolve()
    if not source.is_dir():
        raise RuntimeError('Missing isolated supplier root')
    ownership = json.loads(ledger.read_text()) if ledger.exists() else {}
    planned = []
    seen = set()
    for item in source.rglob('*'):
        if item.is_symlink():
            raise RuntimeError('Supplier contains a link')
        if not item.is_file():
            continue
        rel = item.relative_to(source).as_posix()
        if rel in exclude:
            continue
        key = (prefix + '/' + rel if prefix else rel)
        folded = key.casefold()
        if folded in seen:
            raise RuntimeError('Ambiguous supplier case collision')
        seen.add(folded)
        target = dest.joinpath(*key.split('/'))
        prior = ownership.get(folded)
        if prior:
            if prior['path'] != key or not target.is_file():
                raise RuntimeError('Supplier case or ownership mismatch')
            rule = ALLOWED.get(owner)
            if not rule or prior['owner'] != rule[0] or key not in rule[1]:
                raise RuntimeError('Unexpected supplier collision')
        elif target.exists():
            raise RuntimeError('Untracked existing destination')
        for parent in target.parents:
            if parent == dest:
                break
            if parent.exists() and not parent.is_dir():
                raise RuntimeError('Supplier file/directory collision')
        planned.append((item, target, key, folded))
    if not planned:
        raise RuntimeError('Empty supplier payload')
    # Validate every replacement before making any copies.
    for item, target, key, folded in planned:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, target)
        ownership[folded] = dict(path=key, owner=owner)
    ledger.write_text(json.dumps(ownership, sort_keys=True))
    print(f'SURFACE_ASSEMBLY owner={owner} files={len(planned)} prefix={prefix}', flush=True)

if __name__ == '__main__':
    if os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('CI only')
    p = argparse.ArgumentParser()
    p.add_argument('--source', required=True)
    p.add_argument('--destination', required=True)
    p.add_argument('--ledger', required=True)
    p.add_argument('--owner', required=True)
    p.add_argument('--prefix', default='')
    p.add_argument('--exclude', action='append', default=[])
    args = p.parse_args()
    assemble(args.source, args.destination, args.ledger, args.owner, args.prefix, args.exclude)
