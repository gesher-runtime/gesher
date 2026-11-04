#!/usr/bin/env python3
"""Generates the frameworks/base patch that registers System V JNI entries.

  jni-sysv-patch.py <frameworks/base checkout> > patches/frameworks/base/0002-sysv-jni-tables.patch
  jni-sysv-patch.py <frameworks/base checkout> e2e > patches/frameworks/base/0200-e2e-sysv-jni-tables.patch
  jni-sysv-patch.py <frameworks/base checkout> gl > patches/frameworks/base/0760-gl-sysv-jni-tables.patch

ART enters natives with the System V ABI (docs/decisions/0006-managed-abi.md).
The framework's JNI tables are written by hand, `{"name", "sig", (void*) fn}`,
so they would hand ART Microsoft-ABI functions. In every table of the files
below, each `(void*) fn` and `reinterpret_cast<void*>(fn)` becomes
`GESHER_JNI(fn)`, which core_jni_helpers.h (patch 0001) defines as a System V
thunk under GESHER; a function already declared JNICALL (System V under
GESHER) becomes `GESHER_JNI_SYSV(fn)`, the function itself.

The sources are read from the checkout's HEAD, so the patch is made against
the pristine files whatever is applied to the working tree. FILES lists the
sources cmake/aosp/framework.cmake compiles (plus those of the next step);
add to both together.
"""
import difflib
import re
import subprocess
import sys

FILES = [
    'core/jni/android_content_res_ApkAssets.cpp',
    'core/jni/android_os_MessageQueue.cpp',
    'core/jni/android_os_Parcel.cpp',
    'core/jni/android_os_SystemClock.cpp',
    'core/jni/android_os_SystemProperties.cpp',
    'core/jni/android_os_Trace.cpp',
    'core/jni/android_text_AndroidCharacter.cpp',
    'core/jni/android_util_AssetManager.cpp',
    'core/jni/android_util_Binder.cpp',
    'core/jni/android_util_EventLog.cpp',
    'core/jni/android_util_Log.cpp',
    'core/jni/android_util_StringBlock.cpp',
    'core/jni/android_util_XmlBlock.cpp',
    'core/jni/com_android_internal_util_VirtualRefBasePtr.cpp',
]

# Natives real apps need (cmake/aosp/framework_e2e.cmake, docs/e2e.md), in a patch of their own.
E2E_FILES = [
    'core/jni/android_animation_PropertyValuesHolder.cpp',
    'core/jni/android_database_CursorWindow.cpp',
    'core/jni/android_database_SQLiteConnection.cpp',
    'core/jni/android_database_SQLiteDebug.cpp',
    'core/jni/android_database_SQLiteGlobal.cpp',
    'core/jni/android_database_SQLiteRawStatement.cpp',
]

# The Java OpenGL ES and EGL bindings (cmake/aosp/opengl.cmake, docs/angle.md), in a
# patch of their own. These files (made by frameworks/native/opengl/tools/glgen) do
# not include core_jni_helpers.h: the patch adds it after AndroidRuntime.h.
GL_FILES = [
    'core/jni/android/opengl/util.cpp',
    'core/jni/android_opengl_EGL14.cpp',
    'core/jni/android_opengl_EGL15.cpp',
    'core/jni/android_opengl_EGLExt.cpp',
    'core/jni/android_opengl_GLES10.cpp',
    'core/jni/android_opengl_GLES10Ext.cpp',
    'core/jni/android_opengl_GLES11.cpp',
    'core/jni/android_opengl_GLES11Ext.cpp',
    'core/jni/android_opengl_GLES20.cpp',
    'core/jni/android_opengl_GLES30.cpp',
    'core/jni/android_opengl_GLES31.cpp',
    'core/jni/android_opengl_GLES31Ext.cpp',
    'core/jni/android_opengl_GLES32.cpp',
    'core/jni/com_google_android_gles_jni_EGLImpl.cpp',
    'core/jni/com_google_android_gles_jni_GLImpl.cpp',
]
RUNTIME_INCLUDE = '#include <android_runtime/AndroidRuntime.h>\n'

# The start of a table: `JNINativeMethod name[] = {`, const or static or not.
TABLE = re.compile(r'\bJNINativeMethod\s+\w+\s*\[\s*\w*\s*\]\s*=\s*\{')
# A function name, qualified and with template arguments: ns::fn<jint>.
NAME = r'&?\s*((?:::)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:<[^<>()]*>)?)'
CASTS = [
    re.compile(r'\(\s*void\s*\*\s*\)\s*' + NAME),
    re.compile(r'reinterpret_cast\s*<\s*void\s*\*\s*>\s*\(\s*' + NAME + r'\s*\)'),
]


OVERLOAD = re.compile(r'\(\s*void\s*\*\s*\)\s*(static_cast\s*<[^<>]*>\s*\(\s*[A-Za-z_]\w*\s*\))')


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


def rewrite(path, text, add_include=False):
    if add_include and 'core_jni_helpers.h' not in text and RUNTIME_INCLUDE in text:
        text = text.replace(RUNTIME_INCLUDE,
                            RUNTIME_INCLUDE + '#include "core_jni_helpers.h"  // GESHER_JNI\n', 1)
    out = []
    pos = 0
    tables = 0
    for m in TABLE.finditer(text):
        if m.start() < pos:
            continue
        end = table_end(text, m.end())
        body = text[m.end():end]

        def entry(f):
            # A function declared JNICALL is System V already (libnativehelper
            # patch 0002); it is registered as it is.
            name = f.group(1).split('::')[-1].split('<')[0]
            if re.search(r'\bJNICALL\s+' + re.escape(name) + r'\s*\(', text):
                return 'GESHER_JNI_SYSV(' + f.group(1) + ')'
            return 'GESHER_JNI(' + f.group(1) + ')'

        # An overload picked by a cast: `(void*)static_cast<sig>(fn)`.
        body = OVERLOAD.sub(lambda m: 'GESHER_JNI(' + m.group(1) + ')', body)
        for cast in CASTS:
            body = cast.sub(entry, body)
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
    if len(sys.argv) not in (2, 3) or sys.argv[2:] not in ([], ['e2e'], ['gl']):
        sys.exit(__doc__)
    checkout = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) == 3 else None
    files = {'e2e': E2E_FILES, 'gl': GL_FILES}.get(mode, FILES)
    sys.stdout.reconfigure(newline='\n')  # LF on Windows too
    for path in files:
        old = subprocess.run(['git', '-C', checkout, 'show', 'HEAD:' + path], check=True,
                             capture_output=True).stdout.decode('utf-8')
        new = rewrite(path, old, add_include=mode == 'gl')
        if new == old:
            continue
        sys.stdout.write(f'diff --git a/{path} b/{path}\n')
        sys.stdout.writelines(difflib.unified_diff(
            old.splitlines(keepends=True), new.splitlines(keepends=True),
            f'a/{path}', f'b/{path}', n=3))


if __name__ == '__main__':
    main()
