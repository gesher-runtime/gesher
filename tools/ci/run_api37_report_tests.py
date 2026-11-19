#!/usr/bin/env python3
"""Run the A07 report suites with explicit host coverage and no successful skips."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import sys
import unittest

TESTS = Path(__file__).resolve().parents[1] / 'tests'
MODULES = (
    'test_sdk_platform_diff',
    'test_ndk_platform_diff',
    'test_elf_symbol_inventory',
    'test_source_compat_inventory',
    'test_source_release_diff',
)
LINUX_ELF_TESTS = frozenset(
    'test_elf_symbol_inventory.ElfSymbolInventoryTests.' + name
    for name in (
        'test_compiled_elf32_and_elf64_symbols_objects_tls_and_undefined_refs',
        'test_version_aliases_parent_definitions_and_absolute_symbols',
        'test_versioned_undefined_reference_retains_provider_and_needed_library',
        'test_gnu_unique_object_is_retained',
    )
)
HOSTS = {'linux': 'linux', 'windows': 'win32'}


def flatten(suite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from flatten(test)
        else:
            yield test


def select_tests(profile):
    if profile not in HOSTS:
        raise ValueError(f'Unknown report-test profile: {profile}')
    loader = unittest.TestLoader()
    tests = []
    for module in MODULES:
        selected = list(flatten(loader.discover(str(TESTS), pattern=module + '.py')))
        if loader.errors or not selected:
            raise ValueError(f'Cannot load complete report module {module}: {loader.errors}')
        if any(not test.id().startswith(module + '.') for test in selected):
            raise ValueError(f'Unexpected imported tests in {module}')
        tests.extend(selected)
    identities = [test.id() for test in tests]
    if len(identities) != len(set(identities)):
        raise ValueError('Duplicate report test identities')
    if not LINUX_ELF_TESTS.issubset(identities):
        raise ValueError('The declared Linux ELF tests must exist in the complete suite')
    excluded = LINUX_ELF_TESTS if profile == 'windows' else frozenset()
    return [test for test in tests if test.id() not in excluded], sorted(excluded)


def run_selected(tests, *, stream=None):
    tests = list(tests)
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(unittest.TestSuite(tests))
    # unittest normally treats skipped and expected-failure tests as successful.
    # A missing compiler/Git prerequisite must instead leave this CI gate red.
    success = (bool(tests) and result.testsRun == len(tests) and result.wasSuccessful()
               and not result.skipped and not result.expectedFailures)
    return bool(success), {
        'selected': len(tests), 'executed': result.testsRun,
        'failures': len(result.failures), 'errors': len(result.errors),
        'skipped': len(result.skipped), 'expected_failures': len(result.expectedFailures),
        'unexpected_successes': len(result.unexpectedSuccesses),
        'success': bool(success),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=tuple(HOSTS), required=True)
    args = parser.parse_args(argv)
    if sys.platform != HOSTS[args.profile]:
        parser.error(f'{args.profile} requires {HOSTS[args.profile]}; observed {sys.platform}')
    try:
        tests, excluded = select_tests(args.profile)
    except ValueError as error:
        parser.error(str(error))
    print('A07_REPORT_TEST_SELECTION ' + json.dumps({
        'profile': args.profile, 'platform': sys.platform, 'machine': platform.machine(),
        'python': platform.python_version(), 'modules': list(MODULES),
        'selected': [test.id() for test in tests],
        'excluded_requires_linux_elf_toolchain': excluded,
    }, sort_keys=True), flush=True)
    success, result = run_selected(tests)
    print('A07_REPORT_TEST_RESULT ' + json.dumps(result, sort_keys=True), flush=True)
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
