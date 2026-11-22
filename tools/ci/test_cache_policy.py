"""Only main saves the large or per-commit GitHub Actions caches.

Branch and pull-request copies cannot be shared, and with per-commit or
per-run keys every run adds another entry. Together they pushed the
repository over its cache quota and evicted main's caches, which every
branch restores from.

Governed keys are the AOSP source trees (aosp-*) and any key naming the
commit or run. A step saving one with actions/cache, actions/cache/save or
any version of them must be limited to main, and actions/cache must be
paired with an actions/cache/restore step for other refs. Every
local-build-cache save is governed by that action's own main-only hosted
save. Note that workflows pinning local-build-cache or framework.yml to an
older commit keep that commit's behavior until their pins move.
"""
from pathlib import Path
import re
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
MAIN = "github.ref == 'refs/heads/main'"
OTHER = "github.ref != 'refs/heads/main'"
LOCAL_ACTION = '.github/actions/local-build-cache/action.yml'
# (file, key prefix): reason. Each exception must still save unconditionally.
EXCEPTIONS = {
    ('.github/workflows/arm64-windows.yml', 'ccache-arm64-windows-'):
        'the workflow never runs on a main push; ~25 MB per entry',
}


def expression(text):
    """Compare expressions independent of ${{ }} wrapping and spacing."""
    text = str(text or '').strip()
    match = re.fullmatch(r'\$\{\{(.*)\}\}', text, re.S)
    text = match.group(1) if match else text
    text = re.sub(r'\$\{\{\s*(.*?)\s*\}\}', r'${{ \1 }}', text)
    return ' '.join(text.split())


def kind(step):
    uses = str(step.get('uses', ''))
    phase = str((step.get('with') or {}).get('phase', ''))
    if re.fullmatch(r'actions/cache@\S+', uses):
        return 'cache'
    if re.fullmatch(r'actions/cache/save@\S+', uses):
        return 'save'
    if re.fullmatch(r'actions/cache/restore@\S+', uses):
        return 'restore'
    if re.search(r'(^|/)\.github/actions/local-build-cache(@\S+)?$', uses):
        return {'save': 'local-save', 'restore': 'local-restore'}.get(phase, 'local-other')
    return None


def key(step):
    return expression((step.get('with') or {}).get('key', ''))


def governed(value):
    return value.startswith('aosp-') or '${{ github.sha }}' in value or '${{ github.run_id }}' in value


def steps(document):
    if 'runs' in document:
        yield from document['runs'].get('steps', [])
    for job in (document.get('jobs') or {}).values():
        yield from (job or {}).get('steps', [])


def exception(name, value):
    return next((k for k in EXCEPTIONS if k[0] == name and value.startswith(k[1])), None)


def violations(files):
    found = []
    local = files.get(LOCAL_ACTION)
    for name, document in files.items():
        all_steps = list(steps(document))
        restores = {(key(s), expression(s.get('if'))) for s in all_steps if kind(s) == 'restore'}
        for step in all_steps:
            what, value, condition = kind(step), key(step), expression(step.get('if'))
            label = f'{name}: {step.get("name") or step.get("uses")}'
            if what == 'local-save':
                if local is None:
                    found.append(f'{label}: local-build-cache action is not available to check')
                continue
            if what not in ('cache', 'save', 'restore') or not governed(value):
                continue
            if exception(name, value):
                if what != 'cache' or condition:
                    found.append(f'{label}: exception must stay one unconditional actions/cache step')
                continue
            if what == 'restore':
                if condition != OTHER:
                    found.append(f'{label}: restore is not limited to other refs')
            elif condition != MAIN:
                found.append(f'{label}: saves outside main')
            elif what == 'cache' and (value, OTHER) not in restores:
                found.append(f'{label}: no restore step for other refs')
    if local is not None:
        hosted = [s for s in steps(local) if kind(s) == 'save']
        guard = "runner.environment != 'self-hosted' && inputs.phase == 'save' && " + MAIN
        if len(hosted) != 1 or expression(hosted[0].get('if')) != guard:
            found.append(f'{LOCAL_ACTION}: hosted save is not limited to main')
    for name, prefix in EXCEPTIONS:
        if name in files and not any(key(s).startswith(prefix) for s in steps(files[name])):
            found.append(f'{name}: stale cache policy exception {prefix}')
    return found


def repository_files():
    paths = [*(ROOT / '.github/workflows').glob('*.yml'), *(ROOT / '.github/actions').glob('*/action.yml')]
    return {p.relative_to(ROOT).as_posix(): yaml.safe_load(p.read_text(encoding='utf-8')) for p in sorted(paths)}


def workflow(*step_list, name='x.yml'):
    return {name: {'jobs': {'j': {'steps': list(step_list)}}}}


def cache(uses='actions/cache@v4', condition=None, value='aosp-${{ hashFiles(\'aosp.lock\') }}', **extra):
    step = {'uses': uses, 'with': {'key': value, **extra}}
    if condition is not None:
        step['if'] = condition
    return step


class CachePolicyTest(unittest.TestCase):
    def test_repository_caches_follow_policy(self):
        files = repository_files()
        for required in ('.github/actions/setup-build/action.yml', '.github/workflows/ci.yml'):
            self.assertIn(required, files)
        self.assertEqual(violations(files), [])

    def test_compliant_pair_and_ungoverned_keys(self):
        self.assertEqual(violations(workflow(cache(condition=MAIN),
                                             cache('actions/cache/restore@v4', OTHER))), [])
        self.assertEqual(violations(workflow(cache(condition='${{ ' + MAIN + ' }}'),
                                             cache('actions/cache/restore@v4', '${{github.ref != \'refs/heads/main\'}}'))), [])
        # Fixed-key tool caches are hit exactly by branches and are not governed.
        self.assertEqual(violations(workflow(cache(value='ndk-27.2.12479018'))), [])

    def test_rejects_branch_saves_in_every_spelling(self):
        for value in ('aosp-${{ hashFiles(\'aosp.lock\') }}', 'ccache-x-${{ github.sha }}',
                      'c-${{github.run_id}}', 'c-${{ github.run_id }}'):
            for uses in ('actions/cache@v4', 'actions/cache@v4.2.0', 'actions/cache@' + '0' * 40,
                         'actions/cache/save@v4'):
                with self.subTest(value=value, uses=uses):
                    self.assertTrue(violations(workflow(cache(uses, value=value))))
                    self.assertTrue(violations(workflow(cache(uses, OTHER, value=value))))
            with self.subTest(value=value, case='restore-unlimited'):
                self.assertTrue(violations(workflow(cache('actions/cache/restore@v4', value=value))))

    def test_main_only_cache_needs_branch_restore(self):
        self.assertTrue(violations(workflow(cache(condition=MAIN))))
        self.assertTrue(violations(workflow(cache(condition=MAIN), cache('actions/cache/restore@v4', OTHER, value='aosp-other'))))

    def test_local_build_cache_saves_need_main_only_hosted_save(self):
        base = "runner.environment != 'self-hosted' && inputs.phase == 'save'"
        guarded = {'runs': {'steps': [{'uses': 'actions/cache/save@v4', 'if': base + ' && ' + MAIN}]}}
        unguarded = {'runs': {'steps': [{'uses': 'actions/cache/save@v4', 'if': base}]}}
        bypassed = {'runs': {'steps': [{'uses': 'actions/cache/save@v4', 'if': base + ' && (' + MAIN + ' || true)'}]}}
        for uses in ('./.github/actions/local-build-cache', 'gesher-runtime/gesher/.github/actions/local-build-cache@' + 'a' * 40):
            save = {'uses': uses, 'with': {'phase': 'save', 'key': 'anything'}}
            with self.subTest(uses=uses):
                self.assertEqual(violations({**workflow(save), LOCAL_ACTION: guarded}), [])
                self.assertTrue(violations({**workflow(save), LOCAL_ACTION: unguarded}))
                self.assertTrue(violations({**workflow(save), LOCAL_ACTION: bypassed}))
                self.assertTrue(violations(workflow(save)))

    def test_exceptions_stay_unconditional_and_present(self):
        name, prefix = next(iter(EXCEPTIONS))
        self.assertEqual(violations(workflow(cache(value=prefix + '${{ github.sha }}'), name=name)), [])
        self.assertTrue(violations(workflow(cache(condition=MAIN, value=prefix + '${{ github.sha }}'), name=name)))
        self.assertTrue(violations(workflow(cache(value='other'), name=name)))


if __name__ == '__main__':
    unittest.main()
