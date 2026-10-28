"""Read-only exact-source gate for the separately reviewed witness component."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = 'tools/native3-locks/kernel-source.json'


def git(*arguments):
    return subprocess.check_output(['git', *arguments], cwd=ROOT, text=True).strip()


def main():
    manifest = json.loads((ROOT / MANIFEST).read_text())
    subprocess.run(['git', 'merge-base', '--is-ancestor', manifest['base'], 'HEAD'], cwd=ROOT, check=True)
    assert not git('status', '--porcelain'), 'Clean reviewed source required'
    changed = set(git('diff', '--name-only', manifest['base'], 'HEAD').splitlines())
    code = {path for path in changed if not path.startswith('docs/')}
    assert code == set(manifest['blobs']) | {MANIFEST}, ('Unexpected source delta', code)
    for path, expected in manifest['blobs'].items():
        assert git('hash-object', path) == expected, ('Unreviewed source bytes', path)
    print(f'KERNEL_SOURCE base={manifest["base"]} source={git("rev-parse", "HEAD")} '
          f'pinned_blobs={len(manifest["blobs"])} public_blocking_closed=false', flush=True)


if __name__ == '__main__':
    main()
