"""CI-only bounded source and shared archive-helper identity gate."""
import hashlib
import os
from pathlib import Path
import subprocess

BASE = '7bbccfcddbac826f50b78175f4d23f8702fd7741'
ALLOWED = {
    '.github/workflows/framework.yml', 'tests/apps/sharedlibraryapp/build.sh',
    'tests/dex-graphics/gesher/Aot3SharedLibraryTest.java',
    'tests/shared-libraries/after-res/values/config.xml',
    'tests/windows/tracks/73-aot3-shared-libraries.ps1', 'tools/build-framework-res.sh',
    '.github/workflows/aot3-after-resource-component.yml', 'tools/aot3/preflight.py',
    'tools/aot3/host_payload.py', 'tests/windows/run-aot3-resource-component.ps1',
    'patches/art/0730-aot3-validate-after-shared-library-context.patch',
}
HELPERS = {
    'download-verified-artifact.py': 'cd5cc415e53102c664edac22b1604df639c8804b',
    'assemble-artifacts.py': '8d01e5084c8b0aac7f06bc7eae75cf14e431f5b2',
    'test-verified-artifact.py': 'af7324581ae011f39aacf87d81ae1f44ad4c85f6',
}
assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only'
assert os.environ['GITHUB_REPOSITORY'] == 'gesher-runtime/gesher'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == os.environ['GITHUB_SHA']
subprocess.run(['git', 'merge-base', '--is-ancestor', BASE, 'HEAD'], check=True)
subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--'], check=True)
changed = set(subprocess.check_output(['git', 'diff', '--name-only', BASE, 'HEAD', '--', '.', ':(exclude)docs/**'], text=True).splitlines())
assert changed <= ALLOWED and changed, ('Unreviewed source outside AOT slice', changed - ALLOWED)
for name, expected in HELPERS.items():
    path = Path('tools/surface') / name
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD:' + path.as_posix()], text=True).strip()
    assert actual == expected, (name, actual)
    canonical = subprocess.check_output(['git', 'cat-file', 'blob', expected])
    assert path.read_bytes() == canonical, ('Working bytes differ from canonical helper', name)
    print(f'AOT_HELPER_SOURCE name={name} blob={actual} sha256={hashlib.sha256(canonical).hexdigest()}', flush=True)
print(f'AOT_SOURCE_BOUNDARY base={BASE} source={os.environ["GITHUB_SHA"]} non_doc_paths={len(changed)} other_production_delta=0', flush=True)
