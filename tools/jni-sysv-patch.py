#!/usr/bin/env python3
"""Generates the frameworks/base patch that registers System V JNI entries.

  jni-sysv-patch.py <frameworks/base checkout> > patches/frameworks/base/0002-sysv-jni-tables.patch

ART enters natives with the System V ABI (docs/decisions/0006-managed-abi.md).
The framework's JNI tables are written by hand, `{"name", "sig", (void*) fn}`,
so they would hand ART Microsoft-ABI functions. In every table of the files
below, each `(void*) fn` and `reinterpret_cast<void*>(fn)` becomes
`GESHER_JNI(fn)`, which core_jni_helpers.h (patch 0001) defines as a System V
thunk under GESHER.

The sources are read from the checkout's HEAD, so the patch is made against
the pristine files whatever is applied to the working tree. FILES lists the
sources cmake/aosp/framework.cmake compiles; add to both together.
"""
import difflib
import re
import subprocess
import sys

FILES = [
    'core/jni/android_os_MessageQueue.cpp',
    'core/jni/android_os_SystemClock.cpp',
    'core/jni/android_os_SystemProperties.cpp',
    'core/jni/android_os_Trace.cpp',
    'core/jni/android_text_AndroidCharacter.cpp',
    'core/jni/android_util_EventLog.cpp',
    'core/jni/android_util_Log.cpp',
    'core/jni/com_android_internal_util_VirtualRefBasePtr.cpp',
]

# The start of a table: `JNINativeMethod name[] = {`, const or static or not.
TABLE = re.compile(r'\bJNINativeMethod\s+\w+\s*\[\s*\w*\s*\]\s*=\s*\{')
# A function name, qualified and with template arguments: ns::fn<jint>.
NAME = r'&?\s*((?:::)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:<[^<>()]*>)?)'
CASTS = [
    re.compile(r'\(\s*void\s*\*\s*\)\s*' + NAME),
    re.compile(r'reinterpret_cast\s*<\s*void\s*\*\s*>\s*\(\s*' + NAME + r'\s*\)'),
]


def table_end(text, start):
    """The index just past the brace that closes the one opened at text[start - 1]."""
    depth = 1
    i = start
    while depth:
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        elif c == '"':  # skip string literals (signatures)
            i = text.index('"', i + 1)
        i += 1
    return i


def rewrite(path, text):
    out = []
    pos = 0
    tables = 0
    for m in TABLE.finditer(text):
        if m.start() < pos:
            continue
        end = table_end(text, m.end())
        body = text[m.end():end]
        for cast in CASTS:
            body = cast.sub(lambda f: 'GESHER_JNI(' + f.group(1) + ')', body)
        if re.search(r'void\s*\*\s*>?\s*\)', body):
            sys.exit(f'{path}: a table entry is not a plain function name')
        out.append(text[pos:m.end()])
        out.append(body)
        pos = end
        tables += 1
    if not tables:
        sys.exit(f'{path}: no JNINativeMethod table')
    if 'core_jni_helpers.h' not in text:
        sys.exit(f'{path}: does not include core_jni_helpers.h (GESHER_JNI)')
    out.append(text[pos:])
    return ''.join(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    checkout = sys.argv[1]
    sys.stdout.reconfigure(newline='\n')  # LF on Windows too
    for path in FILES:
        old = subprocess.run(['git', '-C', checkout, 'show', 'HEAD:' + path], check=True,
                             capture_output=True).stdout.decode('utf-8')
        new = rewrite(path, old)
        if new == old:
            continue
        sys.stdout.write(f'diff --git a/{path} b/{path}\n')
        sys.stdout.writelines(difflib.unified_diff(
            old.splitlines(keepends=True), new.splitlines(keepends=True),
            f'a/{path}', f'b/{path}', n=3))


if __name__ == '__main__':
    main()
