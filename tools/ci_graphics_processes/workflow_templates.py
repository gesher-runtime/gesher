"""Exact prospective GraphicsDex caller and immutable bootstrap templates."""
import json
import re
import textwrap

BOOT='''import hashlib,json,os,subprocess,sys
from pathlib import Path
pins=json.loads(os.environ['GRAPHICSDEX_PINS'])
path='tools/ci_graphics_processes/source_guard.py'
raw=subprocess.check_output(['git','cat-file','blob',pins[path]])
assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\\0'+raw).hexdigest()==pins[path]
scope={'__name__':'pinned_source_guard','__file__':str(Path(path).resolve())}
exec(compile(raw,'pinned-source-guard','exec'),scope)
scope['verify_checkout'](Path.cwd(),os.environ['GITHUB_SHA'],pins)
path=Path('tools/ci_graphics_processes/bridge_cli.py').resolve()
sys.argv=[str(path),os.environ['GRAPHICSDEX_MODE']]
exec(compile(path.read_bytes(),str(path),'exec'),{'__name__':'__main__','__file__':str(path)})'''

def caller_bytes(guard):
    assert re.fullmatch('[0-9a-f]{40}',guard)
    boot=textwrap.indent(BOOT,'          ')
    raw='''name: graphicsdex-component
on:
  push:
    branches: [wip/ci-graphics-processes]
    paths: [.github/workflows/graphicsdex-component.yml]
  workflow_dispatch:
permissions:
  contents: read
  actions: read
concurrency:
  group: graphicsdex-${{ github.ref }}
  cancel-in-progress: false
jobs:
  source-authority:
    uses: gesher-runtime/gesher/.github/workflows/graphicsdex-trusted-source.yml@@GUARD@
  fixture-producer:
    needs: source-authority
    runs-on: ubuntu-24.04
    timeout-minutes: 10
    env:
      GH_TOKEN: ${{ github.token }}
      GRAPHICSDEX_PINS: ${{ needs.source-authority.outputs.pins }}
      GRAPHICSDEX_GUARD: ${{ needs.source-authority.outputs.guard }}
      GRAPHICSDEX_CALLER: ${{ needs.source-authority.outputs.caller }}
      GRAPHICSDEX_MODE: produce
    steps:
      - uses: actions/checkout@v4
        with: {fetch-depth: 0, persist-credentials: false}
      - name: Authenticate current producer and emit source manifest
        shell: bash
        run: |
          python -I -B - <<'PY'
@BOOT@
          PY
      - uses: actions/upload-artifact@v4
        with:
          name: graphicsdex-source-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}
          path: ${{ runner.temp }}/graphicsdex-source-manifest.json
          if-no-files-found: error
  fixture:
    needs: [source-authority, fixture-producer]
    strategy:
      fail-fast: false
      matrix: {os: [windows-2022, windows-2025]}
    runs-on: ${{ matrix.os }}
    timeout-minutes: 15
    env:
      GH_TOKEN: ${{ github.token }}
      GRAPHICSDEX_PINS: ${{ needs.source-authority.outputs.pins }}
      GRAPHICSDEX_GUARD: ${{ needs.source-authority.outputs.guard }}
      GRAPHICSDEX_CALLER: ${{ needs.source-authority.outputs.caller }}
    steps:
      - uses: actions/checkout@v4
        with: {fetch-depth: 0, persist-credentials: false}
      - name: Authenticate and verify current producer artifact
        shell: bash
        env: {GRAPHICSDEX_MODE: consume}
        run: |
          python -I -B - <<'PY'
@BOOT@
          PY
      - name: Run original process bootstrap and GraphicsDex cases
        shell: pwsh
        run: ./tools/ci_graphics_processes/run-fixture.ps1
'''
    return raw.replace('@GUARD@',guard).replace('@BOOT@',boot).encode()

def guard_bytes(pins,template):
    assert re.fullmatch('[0-9a-f]{40}',template)
    assert all(re.fullmatch('[0-9a-f]{40}',v)for v in pins.values())
    payload=repr(dict(sorted(pins.items())))
    raw='''name: graphicsdex-trusted-source
on:
  workflow_call:
    outputs:
      pins: {value: '${{ jobs.verify.outputs.pins }}'}
      guard: {value: '${{ jobs.verify.outputs.guard }}'}
      caller: {value: '${{ jobs.verify.outputs.caller }}'}
permissions:
  contents: read
  actions: read
jobs:
  verify:
    runs-on: ubuntu-24.04
    timeout-minutes: 10
    outputs:
      pins: ${{ steps.source.outputs.pins }}
      guard: ${{ steps.source.outputs.guard }}
      caller: ${{ steps.source.outputs.caller }}
    env:
      GH_TOKEN: ${{ github.token }}
      GRAPHICSDEX_CALLER_COMMIT: ${{ vars.GRAPHICSDEX_CALLER_COMMIT }}
    steps:
      - uses: actions/checkout@v4
        with: {fetch-depth: 0, persist-credentials: false}
      - name: Verify immutable source and caller
        id: source
        shell: bash
        run: |
          python -I -B - <<'PY'
          import hashlib,subprocess
          from pathlib import Path
          PINS=@PINS@
          TEMPLATE='@TEMPLATE@'
          path='tools/ci_graphics_processes/source_guard.py'
          raw=subprocess.check_output(['git','cat-file','blob',PINS[path]])
          assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\\0'+raw).hexdigest()==PINS[path]
          scope={'__name__':'pinned_source_guard','__file__':str(Path(path).resolve())}
          exec(compile(raw,'pinned-source-guard','exec'),scope)
          scope['guard_main'](PINS,TEMPLATE)
          PY
      - name: Run pure source and identity controls
        shell: bash
        run: |
          set -e
          for test in test_manifest.py test_role_checks.py test_source_guard.py test_api_context.py test_bridge.py test_workflow_templates.py; do
            python -I -B "tools/ci_graphics_processes/$test"
          done
'''
    return raw.replace('@PINS@',payload).replace('@TEMPLATE@',template).encode()
