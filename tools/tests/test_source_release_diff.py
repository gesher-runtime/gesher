"""Release configuration traces, genuine Git tree binding and offline replay."""

import importlib.util
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("source_release_diff", TOOLS / "source_release_diff.py")
diff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diff)


def declaration(name, value=None, workflow="MANUAL"):
    text = f'name: "{name}" namespace: "android_sdk" workflow: {workflow}\n'
    if value is not None:
        text += 'value { string_value: ' + json.dumps(value) + ' }\n'
    return text.encode()


def assignment(name, value):
    return ('name: ' + json.dumps(name) + ' value { string_value: ' + json.dumps(value) + ' }\n').encode()


def config(name, *parents, variant=False):
    text = 'name: ' + json.dumps(name) + '\n'
    text += ''.join('inherits: ' + json.dumps(p) + '\n' for p in parents)
    if variant:
        text += 'release_config_type: BUILD_VARIANT\n'
    return text.encode()


def default_declarations():
    return {name + '.textproto': declaration(name, value) for name, value in [
        ('RELEASE_PLATFORM_SDK_VERSION', '34'), ('RELEASE_PLATFORM_SDK_VERSION_FULL', None),
        ('RELEASE_PLATFORM_VERSION_CODENAME', 'Development'), ('RELEASE_PLATFORM_VERSION_LAST_STABLE', '14')]}


def config_result(configs, declarations, values, selected='child'):
    mapping = ('aliases { name: "aosp_current" target: ' + json.dumps(selected) + ' }').encode()
    return diff.release_config_profile(configs, declarations, values, mapping, b'BUILD_ID=EXAMPLE.1\n')


def git_archive(directory, name, files):
    """Build a real Git tree/archive without host symlink or chmod privileges."""
    checkout = directory / (name + '-git')
    checkout.mkdir()
    subprocess.run(['git', 'init', '-q', '--object-format=sha1', str(checkout)],
                   check=True, capture_output=True)
    for path, content in sorted(files.items()):
        if isinstance(content, tuple) and content[0] == 'symlink':
            mode, data = '120000', content[1].encode('utf-8')
        else:
            mode = '100755' if path.endswith('.sh') else '100644'
            data = content if isinstance(content, bytes) else content.encode('utf-8')
        blob_id = subprocess.check_output(
            ['git', '-C', str(checkout), 'hash-object', '--no-filters', '-w', '--stdin'],
            input=data).decode('ascii').strip()
        subprocess.run(['git', '-C', str(checkout), 'update-index', '--add',
                        '--cacheinfo', f'{mode},{blob_id},{path}'],
                       check=True, capture_output=True)
    tree = subprocess.check_output(['git', '-C', str(checkout), 'write-tree'], text=True).strip()
    archive = directory / (name + '.tar.gz')
    # Git independently serializes its tree's actual file modes and link targets.
    # The production reader then reconstructs the tree from this archive.
    # archive applies core.autocrlf to text blobs too. Its fixture must retain
    # the exact stored bytes, including an already-CRLF blob, on every host.
    tar = subprocess.check_output(['git', '-C', str(checkout), '-c', 'core.autocrlf=false',
                                   '-c', 'core.eol=lf', 'archive', '--format=tar', tree])
    archive.write_bytes(gzip.compress(tar, mtime=0))
    data = archive.read_bytes()
    return {'filename': archive.name, 'bytes': len(data), 'sha256': diff.sha256(data),
            'git_tree': tree, 'files': len(files), 'url': 'fixture:source', 'repository': 'fixture/source',
            'commit': '1' * 40, 'subtree': 'fixture'}


def blob(directory, name, data):
    data = data.encode() if isinstance(data, str) else data
    (directory / name).write_bytes(data)
    return {'filename': name, 'bytes': len(data), 'sha256': diff.sha256(data),
            'git_blob': diff.git_object('blob', data), 'url': 'fixture:blob'}


class ReleaseConfigTests(unittest.TestCase):
    def test_ordered_parents_conflicts_local_override_and_absent_preview(self):
        configs = {'base.textproto': config('base'), 'left.textproto': config('left', 'base'),
                   'right.textproto': config('right', 'base'), 'child.textproto': config('child', 'left', 'right')}
        values = {}
        for branch, flag, value in [
            ('base', 'RELEASE_PLATFORM_SDK_VERSION', '36'), ('base', 'RELEASE_PLATFORM_SDK_VERSION_FULL', '36.0'),
            ('base', 'RELEASE_PLATFORM_VERSION_CODENAME', 'REL'), ('left', 'RELEASE_PLATFORM_SDK_VERSION_FULL', '36.1'),
            ('right', 'RELEASE_PLATFORM_SDK_VERSION', '37'), ('right', 'RELEASE_PLATFORM_SDK_VERSION_FULL', '37.0'),
            ('child', 'RELEASE_PLATFORM_SDK_VERSION_FULL', '37.1')]:
            values[f'{branch}/{flag}.textproto'] = assignment(flag, value)
        result = config_result(configs, default_declarations(), values)
        self.assertEqual(result['sdk'], {'major': 37, 'minor': 1, 'full_spelling': '37.1', 'status': 'resolved_selected_platform_config'})
        full = result['resolved_fields']['RELEASE_PLATFORM_SDK_VERSION_FULL']
        self.assertEqual(full['default'], {'status': 'absent', 'value': None})
        self.assertEqual([s.get('parent') for s in full['steps'][:2]], ['left', 'right'])
        self.assertTrue(full['steps'][1]['conflict'])
        self.assertEqual(full['steps'][1]['before']['value'], '36.1')
        self.assertEqual(full['steps'][1]['parent_value'], '37.0')
        self.assertEqual(full['steps'][2]['after']['value'], '37.1')
        preview = result['resolved_fields']['RELEASE_PLATFORM_PREVIEW_SDK_INT']
        self.assertEqual(preview['status'], 'undeclared')
        self.assertIsNone(preview['value'])
        self.assertFalse(preview['declaration_present'])
        self.assertFalse(preview['assigned'])

    def test_parent_default_does_not_erase_an_assigned_value(self):
        configs = {'left.textproto': config('left'), 'defaults.textproto': config('defaults'),
                   'child.textproto': config('child', 'left', 'defaults')}
        values = {f'left/{name}.textproto': assignment(name, value) for name, value in [
            ('RELEASE_PLATFORM_SDK_VERSION', '36'), ('RELEASE_PLATFORM_SDK_VERSION_FULL', '36.1')]}
        result = config_result(configs, default_declarations(), values)
        self.assertEqual(result['sdk']['full_spelling'], '36.1')
        major = result['resolved_fields']['RELEASE_PLATFORM_SDK_VERSION']
        self.assertEqual(major['default']['value'], '34')
        self.assertEqual(major['value'], '36')
        self.assertFalse(major['steps'][1]['applied'])
        self.assertEqual(major['steps'][1]['parent_value'], '34')
        self.assertIn('declaration default', major['steps'][1]['reason'])

    def test_noninheriting_flag_and_variant_scope_remain_explicit(self):
        configs = {'base.textproto': config('base'), 'child.textproto': config('child', 'base'),
                   'user.textproto': config('user', variant=True)}
        declarations = default_declarations()
        flag = 'RELEASE_PLATFORM_PREVIEW_SDK_INT'
        declarations[flag + '.textproto'] = declaration(flag, '1', 'MANUAL_NO_INHERIT')
        values = {'base/' + flag + '.textproto': assignment(flag, '0'),
                  'base/RELEASE_PLATFORM_SDK_VERSION.textproto': assignment('RELEASE_PLATFORM_SDK_VERSION', '37'),
                  'base/RELEASE_PLATFORM_SDK_VERSION_FULL.textproto': assignment('RELEASE_PLATFORM_SDK_VERSION_FULL', '37.0'),
                  'user/RELEASE_PLATFORM_SDK_VERSION_FULL.textproto': assignment('RELEASE_PLATFORM_SDK_VERSION_FULL', '37.1')}
        result = config_result(configs, declarations, values)
        preview = result['resolved_fields'][flag]
        self.assertEqual(preview['value'], '1')
        self.assertFalse(preview['assigned'])
        self.assertEqual(preview['steps'][0]['reason'], 'non-inheriting workflow')
        self.assertEqual(result['sdk']['status'], 'unresolved')
        self.assertEqual(result['selected_field_variant_assignments'], ['user/RELEASE_PLATFORM_SDK_VERSION_FULL.textproto'])


@unittest.skipUnless(shutil.which('git'), 'Git is required as an independent tree oracle')
class ArchiveAndReplayTests(unittest.TestCase):
    def test_full_tree_matches_git_including_directory_sort_modes_and_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {'a.java': 'class Plain {}\n', 'a/Value.aidl': 'parcelable Value;\n',
                     'generator.sh': '# source text only\n', 'reference': ('symlink', 'a.java'),
                     'crlf.txt': b'first\r\nsecond\r\n'}
            # A normal Git-for-Windows setting must not change fixture bytes.
            # Symlink metadata must not require host symlink privileges either.
            with patch.dict(os.environ, {'GIT_CONFIG_COUNT': '1',
                                         'GIT_CONFIG_KEY_0': 'core.autocrlf',
                                         'GIT_CONFIG_VALUE_0': 'true'}):
                with patch.object(Path, 'symlink_to', side_effect=PermissionError('fixture has no symlink privilege')):
                    pin = git_archive(root, 'tree', files)
            actual = diff.read_archive(root, pin)
            self.assertEqual(actual['git_tree'], pin['git_tree'])
            self.assertEqual(len(actual['files']), 5)
            modes = {f['path']: f['mode'] for f in actual['files']}
            self.assertEqual(modes['generator.sh'], '100755')
            self.assertEqual(modes['reference'], '120000')
            self.assertEqual(actual['contents']['reference'], b'a.java')
            # These are raw Git object bytes, independent of checkout newline
            # filters, filesystem executable bits or symlink privileges.
            self.assertEqual(actual['contents']['a.java'], files['a.java'].encode('utf-8'))
            self.assertEqual(modes['a.java'], '100644')
            self.assertEqual(modes['a/Value.aidl'], '100644')
            self.assertEqual(actual['contents']['crlf.txt'], files['crlf.txt'])
            altered_pin = dict(pin, sha256='0' * 64)
            with self.assertRaisesRegex(diff.InputError, 'Pinned input differs'):
                diff.read_archive(root, altered_pin)

    def test_aidl_comments_are_byte_changes_without_semantic_claim(self):
        def record(path, content):
            data = content.encode()
            return {'path': path, 'mode': '100644', 'bytes': len(data), 'sha256': diff.sha256(data), 'blob': diff.git_object('blob', data)}
        def profile(files):
            return {'aidl_files': files, 'source_references': {'references': []},
                    'java': {'complete_source_field_denominator': 0, 'complete_resolved_metadata': True}}
        before = profile([record('Same.aidl', 'parcelable Same;'), record('Old.aidl', 'parcelable Old;')])
        after = profile([record('Same.aidl', '// A comment\nparcelable Same;'), record('New.aidl', 'parcelable New;')])
        rows, summary = diff.compare_sources(before, after, [], [])
        self.assertEqual(summary['categories'], {'aidl_source_file:added': 1, 'aidl_source_file:bytes_changed': 1, 'aidl_source_file:removed': 1})
        self.assertEqual({r['category'] for r in rows}, {'aidl_source_file'})
        self.assertFalse(any('abi' in r for r in rows))

    def test_offline_pipeline_replays_identically_with_real_ast_and_git_inputs(self):
        java = os.environ.get('GESHER_SOURCE_DIFF_JAVA') or shutil.which('java')
        if not java:
            self.skipTest('JDK21 is required')
        version = subprocess.run([java, '-m', 'jdk.compiler/com.sun.tools.javac.Main', '-version'], capture_output=True, text=True)
        if version.returncode or not version.stdout.startswith('javac 21.'):
            self.skipTest('JDK21 compiler is required')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock = root / 'aosp.lock'
            lock.write_text('tag android-before\nframeworks/base core/java/\n')
            manifest = {'schema_version': 1, 'comparison_id': 'normal-source-fixture',
                        'project_lock': {'sha256': diff.sha256(lock.read_bytes())},
                        'parser': {'java_version': version.stdout.strip().split()[1]}, 'profiles': []}
            attrs = ['id', 'name', 'disabled', 'loggingOnly', 'noLogging', 'enableAfterTargetSdk', 'enableSinceTargetSdk', 'description', 'overridable']
            xsd = '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"><xs:complexType name="change">' + ''.join(
                f'<xs:attribute name="{a}" type="xs:string"/>' for a in attrs) + '</xs:complexType></xs:schema>'
            for index, tag in enumerate(['android-before', 'android-after-β']):
                label = str(index)
                xml = '<manifest><default revision="refs/tags/' + tag + '"/><project name="platform/frameworks/base" path="frameworks/base"/></manifest>'
                source = 'package sample; import android.compat.annotation.ChangeId; class Changes { @ChangeId static final long FIRST=1L; '
                if index:
                    source += '@ChangeId static final long SECOND=2L; '
                source += '}'
                core_files = {'sample/Changes.java': source, 'sample/Value.aidl': ('// comment\n' if index else '') + 'parcelable Value;'}
                values = {f'release/{name}.textproto': assignment(name, value) for name, value in [
                    ('RELEASE_PLATFORM_SDK_VERSION', '37' if index else '36'),
                    ('RELEASE_PLATFORM_SDK_VERSION_FULL', '37.0' if index else '36.1')]}
                archives = []
                for role, files in [('core_java', core_files), ('release_configs', {'release.textproto': config('release')}),
                                    ('flag_declarations', default_declarations()), ('flag_values', values),
                                    ('compat_schema', {'config/platform-compat-config.xsd': xsd})]:
                    archives.append(dict(git_archive(root, label + '-' + role, files), role=role,
                                         repository='platform/frameworks/base', commit=str(index + 1) * 40))
                manifest['profiles'].append({'id': label, 'tag': tag, 'archives': archives,
                    'blobs': {'release_map': blob(root, label + '-map', 'aliases { name: "aosp_current" target: "release" }'),
                              'build_id': blob(root, label + '-build-id', 'BUILD_ID=SOURCE.' + label + '\n')},
                    'release_manifest': dict(blob(root, label + '-manifest.xml', xml), expected_projects=1),
                    'repository_references': {'frameworks/base': {'path': 'frameworks/base', 'repository': 'platform/frameworks/base',
                        'sparse_selection': ['core/java/'], 'tag': tag, 'commit': str(index + 1) * 40,
                        'tag_object': str(index + 3) * 40, 'annotated_tag': True, 'receipt': {'url': 'fixture:ref'}}}})
            first = diff.run(manifest, root, lock, java, root / 'one')
            diff.run(manifest, root, lock, java, root / 'different-directory')
            ordinary_open = Path.open

            def windows_text_open(path, mode='r', buffering=-1, encoding=None,
                                  errors=None, newline=None):
                # Model legacy Windows text defaults for the actual report writer.
                # Binary source/JSONL writes retain their ordinary byte behavior.
                if 'w' in mode and 'b' not in mode:
                    if newline is None:
                        newline = '\r\n'
                    if encoding in (None, 'locale'):
                        encoding = 'cp1252'
                return ordinary_open(path, mode, buffering, encoding, errors, newline)

            with patch.object(Path, 'open', windows_text_open):
                diff.run(manifest, root, lock, java, root / 'windows')
            for name in ['summary.json', 'profiles.json', 'compat-inventory.jsonl', 'delta.jsonl', 'README.md']:
                expected = (root / 'one' / name).read_bytes()
                self.assertEqual(expected, (root / 'different-directory' / name).read_bytes(), name)
                translated = (root / 'windows' / name).read_bytes()
                self.assertEqual(expected, translated, name)
                self.assertNotIn(b'\r\n', translated, name)
            self.assertIn('android-after-β'.encode('utf-8'), (root / 'windows' / 'README.md').read_bytes())
            self.assertEqual(diff.sha256((root / 'windows' / 'delta.jsonl').read_bytes()), first['delta_sha256'])
            self.assertEqual(diff.sha256((root / 'windows' / 'compat-inventory.jsonl').read_bytes()), first['compat_inventory_sha256'])
            self.assertEqual([p['changeid_occurrences'] for p in first['profiles']], [1, 2])
            self.assertEqual(first['comparison']['resolved_id_additions'], [2])
            self.assertEqual(first['comparison']['categories']['aidl_source_file:bytes_changed'], 1)
            self.assertEqual([p['sdk']['full_spelling'] for p in first['profiles']], ['36.1', '37.0'])


if __name__ == '__main__':
    unittest.main()
