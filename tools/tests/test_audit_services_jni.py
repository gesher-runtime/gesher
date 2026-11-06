import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'service_audit', Path(__file__).parents[1] / 'audit_services_jni.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


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
