import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

spec = importlib.util.spec_from_file_location(
    'service_audit', Path(__file__).parents[1] / 'audit_services_jni.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def method(name, descriptor, flags=audit.members.PUBLIC):
    return audit.members.Member('method', name, descriptor, flags)


def cls(name, parent='java.lang.Object', methods=(), interfaces=(), flags=audit.members.PUBLIC):
    return audit.members.Class(name, flags, parent, tuple(interfaces), {m.key: m for m in methods})


class ServiceDescriptorTests(unittest.TestCase):
    def setUp(self):
        self.api = [method('call', '(I)Z'), method('call', '(Ljava/lang/String;)Z'),
                    method('value', '()Ljava/lang/Number;'), method('array', '([[Ljava/lang/String;)V')]
        self.framework = {
            'java.lang.Object': cls('java.lang.Object', parent=None),
            'sample.Api': cls('sample.Api', methods=[method(m.name, m.descriptor, audit.members.PUBLIC | audit.members.ABSTRACT)
                                                    for m in self.api],
                              flags=audit.members.PUBLIC | audit.members.INTERFACE | audit.members.ABSTRACT),
            'sample.Api$Default': cls('sample.Api$Default', methods=self.api, interfaces=['sample.Api']),
        }
        self.source = [{'class': 'Service', 'binary_class': 'sample.Service', 'interface': 'sample.Api',
                        'base_kind': 'Default', 'source': {'path': 'Service.java', 'line': 1},
                        'explicit_methods': [{'name': 'call', 'arity': 1}]}]

    def compare(self, methods=(), app=None, framework=None):
        if app is None:
            app = {'sample.Service': cls('sample.Service', 'sample.Api$Default', methods)}
        classes, gaps = audit.compare_services(self.source, framework or self.framework, app)
        return next(c for c in classes if c['binary_class'] == 'sample.Service'), gaps, classes

    def test_same_name_and_arity_do_not_hide_a_different_overload(self):
        service, gaps, _ = self.compare([self.api[0]])
        self.assertEqual({g['descriptor'] for g in gaps if g['name'] == 'call'}, {'(Ljava/lang/String;)Z'})
        self.assertEqual(service['inherited_default_count'], 3)
        self.assertEqual(service['comparison_status'], 'measured_compiled_descriptors')

    def test_return_type_is_part_of_descriptor_and_bridge_supplies_override(self):
        narrow = method('value', '()Ljava/lang/Integer;')
        _, gaps, _ = self.compare([narrow])
        self.assertIn('value()Ljava/lang/Number;', {g['signature'] for g in gaps})
        bridge = method('value', '()Ljava/lang/Number;', audit.members.PUBLIC | 0x40 | 0x1000)
        service, gaps, _ = self.compare([narrow, bridge])
        self.assertNotIn('value', {g['name'] for g in gaps})
        row = next(r for r in service['method_resolutions'] if r['name'] == 'value')
        self.assertTrue(row['compiler_bridge'])

    def test_array_dimensions_are_not_collapsed(self):
        _, gaps, _ = self.compare([method('array', '([Ljava/lang/String;)V')])
        self.assertIn('array([[Ljava/lang/String;)V', {g['signature'] for g in gaps})
        _, gaps, _ = self.compare([self.api[3]])
        self.assertNotIn('array', {g['name'] for g in gaps})

    def test_indirect_subclasses_resolve_inherited_concrete_implementation(self):
        app = {'sample.Parent': cls('sample.Parent', 'sample.Api$Default', [self.api[0]]),
               'sample.Service': cls('sample.Service', 'sample.Parent'),
               'sample.Outer$1': cls('sample.Outer$1', 'sample.Parent')}
        service, _, classes = self.compare(app=app)
        row = next(r for r in service['method_resolutions'] if r['signature'] == 'call(I)Z')
        self.assertEqual(row['status'], 'compiled_implementation_unverified')
        self.assertEqual(row['resolved_owners'], ['sample.Parent'])
        self.assertEqual(row['implementation_location'], 'inherited')
        self.assertEqual({c['binary_class'] for c in classes}, set(app))

    def test_missing_app_input_and_missing_service_are_unmeasured(self):
        for app, expected in ((None, 'app_class_input_not_provided'), ({}, 'compiled_service_class_unavailable')):
            with self.subTest(app=app):
                classes, gaps = audit.compare_services(self.source, self.framework, app)
                self.assertEqual(classes[0]['comparison_status'], expected)
                self.assertIsNone(classes[0]['inherited_default_count'])
                self.assertEqual(gaps, [])

    def test_missing_default_is_not_a_zero_gap_result(self):
        classes, gaps = audit.compare_services(self.source, {}, {})
        self.assertEqual(classes[0]['comparison_status'], 'framework_default_class_unavailable')
        self.assertIsNone(classes[0]['interface_method_count'])
        self.assertIsNone(classes[0]['inherited_default_count'])
        self.assertEqual(gaps, [])

    def test_missing_parent_is_distinguished_from_missing_override(self):
        service, gaps, _ = self.compare(app={'sample.Service': cls('sample.Service', 'sample.Unavailable')})
        self.assertEqual(service['comparison_status'], 'compiled_service_ancestry_unresolved')
        self.assertIsNone(service['inherited_default_count'])
        self.assertEqual(gaps, [])

    def test_missing_interface_keeps_partial_rows_but_not_complete_count(self):
        framework = dict(self.framework)
        framework['sample.Api'] = cls('sample.Api', methods=self.api, interfaces=['sample.Unavailable'],
                                      flags=audit.members.PUBLIC | audit.members.INTERFACE)
        service, gaps, _ = self.compare(framework=framework)
        self.assertEqual(service['unavailable_surface_classes'], ['sample.Unavailable'])
        self.assertIsNone(service['interface_method_count'])
        self.assertIsNone(service['inherited_default_count'])
        self.assertEqual(len(gaps), 4)

    def test_signature_dependency_absence_is_separate_from_dispatch(self):
        service, gaps, _ = self.compare([self.api[0]])
        row = next(g for g in gaps if g['name'] == 'call')
        self.assertEqual(row['descriptor_types_not_in_inputs'], ['java.lang.String'])
        self.assertEqual(service['comparison_status'], 'measured_compiled_descriptors')

    def test_non_public_static_or_abstract_method_cannot_count_as_implementation(self):
        for flags in (audit.members.PRIVATE, audit.members.PUBLIC | audit.members.STATIC,
                      audit.members.PUBLIC | audit.members.ABSTRACT):
            with self.subTest(flags=flags):
                service, _, _ = self.compare([method('call', '(I)Z', flags)])
                row = next(r for r in service['method_resolutions'] if r['signature'] == 'call(I)Z')
                self.assertEqual(row['status'], 'compiled_method_not_concrete_public_instance')
                self.assertIsNone(service['inherited_default_count'])

    def test_framework_provider_wins_over_duplicate_app_class(self):
        app = {'sample.Api$Default': cls('sample.Api$Default', methods=[]),
               'sample.Service': cls('sample.Service', 'sample.Api$Default')}
        service, gaps, classes = self.compare(app=app)
        self.assertEqual(service['inherited_default_count'], 4)
        self.assertTrue(all(g['resolved_providers'] == ['framework'] for g in gaps))
        self.assertEqual({c['binary_class'] for c in classes}, {'sample.Service'})

    def test_stub_abstract_contract_is_not_an_inherited_default(self):
        self.framework['sample.Api$Stub'] = cls('sample.Api$Stub', interfaces=['sample.Api'],
                                               flags=audit.members.PUBLIC | audit.members.ABSTRACT)
        for implemented in (self.api, self.api[:1]):
            with self.subTest(implemented=len(implemented)):
                service, gaps, _ = self.compare(app={'sample.Service': cls('sample.Service', 'sample.Api$Stub', implemented)})
                self.assertEqual(service['base_kind'], 'Stub')
                self.assertEqual(gaps, [])
                self.assertEqual(service['inherited_default_count'], 0 if len(implemented) == 4 else None)

    def test_cycle_is_rejected_without_hanging(self):
        app = {'sample.Service': cls('sample.Service', 'sample.Parent'),
               'sample.Parent': cls('sample.Parent', 'sample.Service')}
        with self.assertRaisesRegex(ValueError, 'cyclic'):
            self.compare(app=app)

    def test_anonymous_object_method_is_not_an_enclosing_service_override(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'app/java/sample/Service.java'
            source.parent.mkdir(parents=True)
            source.write_text('''package sample;
import sample.Api;
class Outer {
    static class Service extends Api.Default {
        Object helper() {
            return new Object() { public void call(String value) {} };
        }
        public boolean call(int value) { return true; }
    }
}''', encoding='utf8')
            classes, _ = audit.source_service_classes(root, lambda p, text, pos: {'path': str(p), 'line': 1})
            self.assertEqual(len(classes), 1)
            self.assertEqual(classes[0]['binary_class'], 'sample.Outer$Service')
            self.assertEqual([(m['name'], m['return_type_source']) for m in classes[0]['explicit_methods']],
                             [('call', 'boolean')])


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'JDK required for isolated compiled fixture')
class CompiledServiceTests(unittest.TestCase):
    def test_real_class_files_overloads_bridges_and_inherited_dispatch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'src' / 'sample'
            source.mkdir(parents=True)
            (source / 'Api.java').write_text('''package sample;
public interface Api {
    boolean call(int value); boolean call(String value); Number value(); void array(String[][] values);
    class Default implements Api {
        public boolean call(int value) { return false; }
        public boolean call(String value) { return false; }
        public Number value() { return null; }
        public void array(String[][] values) { }
    }
}''', encoding='utf8')
            (source / 'Parent.java').write_text('''package sample;
public class Parent extends Api.Default {
    public boolean call(int value) { return true; }
    public Integer value() { return 1; }
}''', encoding='utf8')
            (source / 'Child.java').write_text('package sample; public class Child extends Parent {}', encoding='utf8')
            (source / 'ApiGeneric.java').write_text('''package sample;
public interface ApiGeneric<T> {
    T echo(T value);
    class Default<T> implements ApiGeneric<T> { public T echo(T value) { return null; } }
}''', encoding='utf8')
            (source / 'StringService.java').write_text('''package sample;
public class StringService extends ApiGeneric.Default<String> {
    public String echo(String value) { return value; }
}''', encoding='utf8')
            (source / 'Probe.java').write_text('''package sample;
public class Probe {
    public static void main(String[] args) throws Exception {
        System.out.println(Child.class.getMethod("call", int.class).getDeclaringClass().getName());
        System.out.println(Child.class.getMethod("call", String.class).getDeclaringClass().getName());
    }
}''', encoding='utf8')
            output = root / 'classes'
            subprocess.run(['javac', '-d', str(output), *map(str, source.glob('*.java'))], check=True, capture_output=True)
            for name, select in (('framework', lambda p: p.name.startswith('Api')), ('app', lambda p: not p.name.startswith('Api'))):
                with zipfile.ZipFile(root / (name + '.zip'), 'w') as archive:
                    for path in output.rglob('*.class'):
                        if select(path):
                            archive.write(path, path.relative_to(output).as_posix())
            framework, framework_input = audit.members.archive_members(root / 'framework.zip', 'sdk', 'framework-classes')
            app, app_input = audit.members.archive_members(root / 'app.zip', 'sdk', 'app-classes')
            classes, gaps = audit.compare_services([], framework, app)
            child = next(c for c in classes if c['binary_class'] == 'sample.Child')
            self.assertEqual(child['inherited_default_count'], 2)
            self.assertEqual({g['signature'] for g in gaps if g['binary_class'] == 'sample.Child'},
                             {'call(Ljava/lang/String;)Z', 'array([[Ljava/lang/String;)V'})
            rows = {r['signature']: r for r in child['method_resolutions']}
            self.assertTrue(rows['value()Ljava/lang/Number;']['compiler_bridge'])
            generic = next(c for c in classes if c['binary_class'] == 'sample.StringService')
            self.assertEqual(generic['inherited_default_count'], 0)
            self.assertEqual(generic['method_resolutions'][0]['signature'], 'echo(Ljava/lang/Object;)Ljava/lang/Object;')
            self.assertTrue(generic['method_resolutions'][0]['compiler_bridge'])
            actual = subprocess.run(['java', '-cp', str(output), 'sample.Probe'], check=True, capture_output=True, text=True)
            self.assertEqual(actual.stdout.splitlines(), [rows['call(I)Z']['resolved_owners'][0],
                                                        rows['call(Ljava/lang/String;)Z']['resolved_owners'][0]])
            self.assertEqual(framework_input['class_definitions'], 4)
            self.assertEqual(app_input['class_definitions'], 4)
            self.assertNotEqual(framework_input['sha256'], app_input['sha256'])


class JniEvidenceTests(unittest.TestCase):
    def test_reviewed_alternate_registration_is_source_evidence(self):
        locations = [{'path': 'compat/framework/memory_file.cc', 'line': 7}]
        symbol = 'register_android_os_MemoryFile'
        result = audit.jni_finding({
            'symbol': symbol, 'local_symbol_references': [],
            'local_registration_aliases': [{
                'symbol': audit.REGISTRATION_ALIASES[symbol],
                'source_references': locations,
            }],
            'aosp_source': {'path': 'AndroidRuntime.cpp', 'line': 1},
        })
        self.assertEqual(result['status'], 'implemented_unverified')
        self.assertEqual(result['evidence'], locations)
        self.assertNotIn('negative_search_scope', result)

    def test_new_local_registration_removes_stale_missing_claim(self):
        locations = [{'path': 'compat/framework/memory_file.cc', 'line': 14}]
        result = audit.jni_finding({
            'symbol': 'register_android_os_MemoryFile',
            'local_symbol_references': locations,
            'aosp_source': {'path': 'AndroidRuntime.cpp', 'line': 1},
        })
        self.assertEqual(result['status'], 'implemented_unverified')
        self.assertEqual(result['evidence'], locations)
        self.assertNotIn('negative_search_scope', result)
        self.assertIn('runtime evidence', result['risk'])

    def test_missing_reference_retains_upstream_evidence_and_search_scope(self):
        upstream = {'path': 'AndroidRuntime.cpp', 'line': 1}
        result = audit.jni_finding({
            'symbol': 'register_android_os_MemoryFile',
            'local_symbol_references': [], 'aosp_source': upstream,
        })
        self.assertEqual(result['status'], 'missing')
        self.assertEqual(result['evidence'], upstream)
        self.assertIn('compat/**/*.cc', result['negative_search_scope'])
        self.assertIn('aliases', result['risk'])


if __name__ == '__main__':
    unittest.main()
