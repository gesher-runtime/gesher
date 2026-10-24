#!/usr/bin/env python3
"""Generates libandroid_runtime's stand-in tables for device-only natives.

  gen_device_standins.py <out.cc> <class>=<jni source>[:<table>] ...

<class> is a JNI class name, with '.' before a nested class's name
(android/graphics/Foo.Bar); <table> defaults to gMethods.

Some of the framework's natives talk to what only a device has:
SurfaceFlinger (SurfaceControl and its Transactions, SurfaceSession),
BufferQueue's BLAST adapter (BLASTBufferQueue), InputDispatcher's channels
(InputChannel, InputEventReceiver), the display's overlays
(OverlayProperties) and Perfetto's producer (android.tracing.perfetto).
Gesher has no compositor; the framework still calls them when a window is
laid out (ViewRootImpl), so each native of the listed tables is registered
with an answer that holds no state:
zero, false, null or 0.0 (by its JNI return type). The few that must create
something or return a function are hand-written in device_standins.cc and
named in SPECIALS; the table, with each native's name and signature, is read
from the JNI source itself, so it matches framework.jar.

The answers are System V functions that take no parameters: ART calls natives
with the System V ABI (docs/decisions/0006-managed-abi.md), where the caller
pops the arguments, so one function answers every signature; floating-point
results come back in xmm0, the others in rax.
"""
import re
import sys

ENTRY = re.compile(r'\{\s*"(\w+)"\s*,\s*((?:"[^"]*"\s*)+),')
TABLE = r'\b{}\s*\[\s*\]\s*=\s*\{{'

# (class, native name[, signature]) -> hand-written function in
# device_standins.cc, a System V function with C linkage named
# gesher_device_<name>. A key with a signature takes precedence over one
# without.
SPECIALS = {
    # Handles Java checks for non-zero (SurfaceControl.isValid(), and
    # NativeAllocationRegistry, which rejects 0), with a function to free them.
    ('android/view/SurfaceControl', 'nativeCreate'): 'Standin_create',
    ('android/view/SurfaceControl', 'nativeReadFromParcel'): 'Standin_create',
    ('android/view/SurfaceControl', 'nativeCopyFromSurfaceControl'): 'Standin_create',
    ('android/view/SurfaceControl', 'nativeMirrorSurface'): 'Standin_create',
    ('android/view/SurfaceControl', 'nativeGetNativeSurfaceControlFinalizer'):
        'Standin_getFreeFunction',
    ('android/view/SurfaceControl', 'nativeCreateTransaction'): 'Standin_create',
    ('android/view/SurfaceControl', 'nativeGetNativeTransactionFinalizer'):
        'Standin_getFreeFunction',
    ('android/view/SurfaceControl', 'nativeCreateJankDataListenerWrapper'): 'Standin_create',
    ('android/view/SurfaceControl', 'nativeGetJankDataListenerWrapperFinalizer'):
        'Standin_getFreeFunction',
    ('android/view/SurfaceControl', 'nativeCreateTpc'): 'Standin_create',
    ('android/view/SurfaceControl', 'getNativeTrustedPresentationCallbackFinalizer'):
        'Standin_getFreeFunction',
    ('android/view/SurfaceControl', 'nativeGetDefaultApplyToken'): 'Standin_zero',
    ('android/view/SurfaceSession', 'nativeCreate'): 'Standin_create',
    ('android/graphics/BLASTBufferQueue', 'nativeCreate'): 'Standin_create',
    # The window's Surface (docs/graphics.md): a provider the window code
    # installs, else null.
    ('android/graphics/BLASTBufferQueue', 'nativeGetSurface'): 'Blast_getSurface',
    # Handles carry a tag (the Gesher window a SurfaceControl stands for,
    # gesher_device_standins.h) from the SurfaceControl the window session
    # made to its copies and to the BLAST adapter that draws into it.
    ('android/view/SurfaceControl', 'nativeCopyFromSurfaceControl'): 'SurfaceControl_copy',
    ('android/view/SurfaceControl', 'nativeGetHandle'): 'Standin_tag',
    ('android/graphics/BLASTBufferQueue', 'nativeUpdate'): 'Blast_update',
    ('android/graphics/BLASTBufferQueue', 'nativeIsSameSurfaceControl'):
        'Blast_isSameSurfaceControl',
    # Input channels (InputDispatcher's sockets) and receivers: none yet; a
    # window added with one gets a channel that never delivers.
    ('android/view/InputChannel', 'nativeGetFinalizer'): 'Standin_getFreeFunction',
    # The display's overlay support (libgui): a default that supports nothing.
    ('android/hardware/OverlayProperties', 'nCreateDefault'): 'Standin_create',
    ('android/hardware/OverlayProperties', 'nGetDestructor'): 'Standin_getFreeFunction',
    ('android/hardware/OverlayProperties', 'nReadOverlayPropertiesFromParcel'):
        'Standin_create',
    # Perfetto's producer and data sources (ViewRootImpl starts ProtoLog): none;
    # nothing is traced.
    ('android/tracing/perfetto/DataSource', 'nativeCreate'): 'Standin_create',
    ('android/tracing/perfetto/DataSource', 'nativeGetFinalizer'): 'Standin_getFreeFunction',
}


def entries(path, table):
    text = open(path, encoding='utf-8').read()
    m = re.search(TABLE.format(table), text)
    if not m:
        sys.exit(f'{path}: no table {table}')
    # The table ends at a '};' on its own line, or right after the last entry.
    end = re.compile(r'\n\s*\};|(?<=\})\};').search(text, m.end()).start()
    out = []
    for e in ENTRY.finditer(text, m.end(), end):
        out.append((e.group(1), ''.join(re.findall(r'"([^"]*)"', e.group(2)))))
    if not out:
        sys.exit(f'{path}: table {table} has no entries')
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = [
        '// Generated by compat/framework/gen_device_standins.py; do not edit.',
        '#include <jni.h>',
        '',
        '// Defined in device_standins.cc; only their addresses are taken here.',
        'extern "C" {',
        'void gesher_device_Standin_zero();',
        'void gesher_device_Standin_zeroFloat();',
    ]
    for symbol in sorted(set(SPECIALS.values()) - {'Standin_zero', 'Standin_zeroFloat'}):
        out.append(f'void gesher_device_{symbol}();')
    out.append('}')
    out.append('')
    out.append('namespace gesher_device {')
    out.append('')
    classes = []
    for arg in sys.argv[2:]:
        cls, source = arg.split('=', 1)
        # Nested classes are given with '.', as '$' does not survive build
        # files and shells.
        cls = cls.replace('.', '$')
        table = 'gMethods'
        m = re.match(r'(.*):([A-Za-z_]\w*)$', source)
        if m:
            source, table = m.group(1), m.group(2)
        var = 'k' + re.sub(r'\W', '_', cls)
        out.append(f'static const JNINativeMethod {var}[] = {{')
        for name, sig in entries(source, table):
            ret = sig[sig.index(')') + 1]
            fn = SPECIALS.get((cls, name, sig)) or SPECIALS.get((cls, name)) or (
                'Standin_zeroFloat' if ret in 'FD' else 'Standin_zero')
            out.append(f'    {{"{name}", "{sig}", reinterpret_cast<void*>(&gesher_device_{fn})}},')
        out.append('};')
        classes.append((cls, var))
    out.append('')
    out.append('struct StandinTable {')
    out.append('  const char* className;')
    out.append('  const JNINativeMethod* methods;')
    out.append('  int count;')
    out.append('};')
    out.append('')
    out.append('extern const StandinTable kStandinTables[] = {')
    for cls, var in classes:
        out.append(f'    {{"{cls}", {var}, sizeof({var}) / sizeof({var}[0])}},')
    out.append('};')
    out.append(f'extern const int kStandinTableCount = {len(classes)};')
    out.append('')
    out.append('}  // namespace gesher_device')
    with open(sys.argv[1], 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
