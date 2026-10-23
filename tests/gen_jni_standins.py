#!/usr/bin/env python3
"""Completes the tests' stand-in framework classes with their native methods.

  gen_jni_standins.py <frameworks/base> <out-dir> <template.java.in>...

The tests (tests/dex-natives) run libandroid_runtime's natives without
framework.jar, on stand-in classes. RegisterNatives needs every method of a
JNI table to exist, so a stand-in for a class with many natives (AssetManager
has 57) declares them from the table itself: each template has a line

  // @natives core/jni/android_util_AssetManager.cpp gAssetManagerMethods

which becomes one `static native` declaration per table entry, with the
types of its JNI signature, and @CriticalNative when the C++ function takes
CRITICAL_JNI_PARAMS (core_jni_helpers.h). ART's RegisterNatives matches name
and signature only, so static and instance natives are both declared static.
The output is <out-dir>/<template's path below its `tests/dex-natives-in`>
without `.in`.
"""
import os
import re
import sys

MARKER = re.compile(r'^([ \t]*)// @natives (\S+) (\w+)[ \t]*$', re.M)
ENTRY = re.compile(
    r'\{\s*"(\w+)"\s*,\s*((?:"[^"]*"\s*)+),\s*'
    r'(?:GESHER_JNI\(\s*([\w:]+)(?:<[^>]*>)?\s*\)'
    r'|\(void\s*\*\)\s*&?\s*([\w:]+)(?:<[^>]*>)?'
    r'|reinterpret_cast<void\s*\*>\s*\(\s*&?\s*([\w:]+)(?:<[^>]*>)?\s*\))\s*\}')
PRIMITIVES = {'Z': 'boolean', 'B': 'byte', 'C': 'char', 'S': 'short', 'I': 'int',
              'J': 'long', 'F': 'float', 'D': 'double', 'V': 'void'}


def java_type(sig, i):
    """The Java type of the JNI type at sig[i], and the index after it."""
    dims = 0
    while sig[i] == '[':
        dims += 1
        i += 1
    if sig[i] == 'L':
        end = sig.index(';', i)
        name = sig[i + 1:end].replace('/', '.').replace('$', '.')
        i = end + 1
    else:
        name = PRIMITIVES[sig[i]]
        i += 1
    return name + '[]' * dims, i


def declaration(name, sig, critical):
    params = []
    i = 1
    while sig[i] != ')':
        t, i = java_type(sig, i)
        params.append(f'{t} p{len(params)}')
    ret, _ = java_type(sig, i + 1)
    annotation = '@dalvik.annotation.optimization.CriticalNative ' if critical else ''
    return f'{annotation}public static native {ret} {name}({", ".join(params)});'


def natives(fw, source, table):
    text = open(os.path.join(fw, source), encoding='utf-8').read()
    m = re.search(r'\b' + table + r'\s*\[\s*\]\s*=\s*\{', text)
    if not m:
        sys.exit(f'{source}: no table {table}')
    end = text.index('\n};', m.end())
    out = []
    for e in ENTRY.finditer(text, m.end(), end):
        name, sig = e.group(1), ''.join(re.findall(r'"([^"]*)"', e.group(2)))
        fn = (e.group(3) or e.group(4) or e.group(5)).split('::')[-1]
        critical = re.search(r'\b' + fn + r'\s*\(\s*CRITICAL_JNI_PARAMS', text) is not None
        out.append(declaration(name, sig, critical))
    if not out:
        sys.exit(f'{source}: table {table} has no entries')
    return out


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    fw, out_dir = sys.argv[1], sys.argv[2]
    for template in sys.argv[3:]:
        text = open(template, encoding='utf-8').read()

        def expand(m):
            return '\n'.join(m.group(1) + d for d in natives(fw, m.group(2), m.group(3)))

        path = '/' + os.path.abspath(template).replace('\\', '/')
        rel = path.split('/dex-natives-in/', 1)[1][:-len('.in')]
        dest = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'w', encoding='utf-8', newline='\n') as f:
            f.write(MARKER.sub(expand, text))


if __name__ == '__main__':
    main()
