# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Distinct fork diagnostic receipts using the actual minimum supplier facts."""
import json
from pathlib import Path
import sys

import minimum_receipt as facts

sys.path.insert(0, str(facts.ROOT / 'tools/toolchain'))
import fork_tcp_context as identity

SCHEMA = 'gesher-minimum-fork-diagnostic-v1'
ROLE = 'gesher-winpthreads-fork-diagnostic-v1'
PUBLISHER_ROLE = 'publisher-reference-fork-diagnostic-v1'
RECEIPT = 'gesher-winpthreads-fork-diagnostic.json'


def original(llvm, source, output, descriptor):
    identity.assert_current(descriptor, 'compile')
    assert not (llvm / RECEIPT).exists()
    value = dict(context={k: descriptor['context'][k] for k in identity.FIELDS[:4]},
                 role=PUBLISHER_ROLE, evidence_schema=SCHEMA, diagnostic=descriptor,
                 **facts.original_facts(llvm, source))
    output.write_text(json.dumps(value, sort_keys=True, indent=2))


def supplier(source, llvm, original_file, descriptor):
    identity.assert_current(descriptor, 'compile')
    facts.input_facts(source, patched=True)
    context = {k: descriptor['context'][k] for k in identity.FIELDS[:4]}
    baseline = json.loads(original_file.read_text())
    assert baseline['context'] == context and baseline['role'] == PUBLISHER_ROLE
    assert baseline['evidence_schema'] == SCHEMA and baseline['diagnostic'] == descriptor
    assert not (llvm / 'gesher-winpthreads-authority.json').exists()
    assert not (llvm / RECEIPT).exists()
    value = dict(context, role=ROLE, evidence_schema=SCHEMA, diagnostic=descriptor,
                 **facts.supplier_facts(source, llvm, baseline))
    payload = json.dumps(value, sort_keys=True, indent=2).encode('utf-8')
    (llvm / RECEIPT).write_bytes(payload)
    # The original canonical transport rejects this distinct prefix. Its
    # bounded chunking/hashes are reusable data framing, not an authority grant.
    for line in facts.receipt_lines(payload):
        framed = 'FORK_DIAGNOSTIC_' + line
        assert framed.isascii() and len(framed) <= 4096
        print(framed, flush=True)


def main():
    operation, *arguments = sys.argv[1:]
    expected = {'inputs': 1, 'patched': 1, 'original': 3, 'profile': 1, 'supplier': 3}
    assert operation in expected and len(arguments) == expected[operation]
    descriptor = identity.current('compile')
    paths = [Path(value).resolve(strict=True) if operation != 'original' or index != 2 else Path(value)
             for index, value in enumerate(arguments)]
    if operation in ('inputs', 'patched'):
        facts.input_facts(paths[0], patched=operation == 'patched')
    elif operation == 'original':
        original(*paths, descriptor)
    elif operation == 'supplier':
        supplier(*paths, descriptor)
    else:
        facts.profile_facts(paths[0])


if __name__ == '__main__':
    main()
