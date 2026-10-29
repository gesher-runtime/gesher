"""Read-only exact-source gate. Run only the reviewed component in future CI."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = 'tools/native3-locks/blocking-source.json'


def git(*arguments):
    return subprocess.check_output(['git', *arguments], cwd=ROOT, text=True).strip()


def main():
    manifest = json.loads((ROOT / MANIFEST).read_text())
    base = manifest['base']
    subprocess.run(['git', 'merge-base', '--is-ancestor', base, 'HEAD'], cwd=ROOT, check=True)
    assert git('status', '--porcelain') == '', 'Source checkpoint must be clean'
    changed = set(git('diff', '--name-only', base, 'HEAD').splitlines())
    code = {path for path in changed if not path.startswith('docs/')}
    assert code == set(manifest['blobs']) | {MANIFEST}, ('Unexpected source delta', code)
    for path, expected in manifest['blobs'].items():
        assert git('hash-object', path) == expected, ('Unreviewed source bytes', path)
    # No qualified tests, signals, descriptor/stdio ownership, Exec, Binder,
    # bionic, AOT or accepted full suppliers are allowed through this guard.
    print(f'BLOCKING_SOURCE base={base} source={git("rev-parse", "HEAD")} '
          f'pinned_blobs={len(manifest["blobs"])} public_blocking_closed=false', flush=True)


if __name__ == '__main__':
    main()
