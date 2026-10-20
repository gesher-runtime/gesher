#!/usr/bin/env python3
"""Checks that every C++/assembly boundary in ART uses the System V ABI.

  abi-check.py <build dir> <AOSP dir>

ART's assembly calls C++ with System V arguments; on Windows the C++ side
must say so (ART_SYSV_ABI, docs/decisions/0006-managed-abi.md). A missing
annotation compiles and links but corrupts arguments, so this check:

1. takes every function the assembly objects call and that C++ defines,
2. finds the object that defines it and that object's source file,
3. requires an `extern "C" ART_SYSV_ABI` declaration of it there (for
   names made by token pasting: every `extern "C"` in the file annotated),

and checks the assembly functions that C++ calls directly (CALLED_FROM_CPP).
Exits non-zero if anything is missing.
"""
import glob
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('coff', os.path.join(HERE, 'coff-symbols.py'))
coff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coff)

# Data, not functions.
DATA = {'gesher_thread_self_slot', '_ZN3art7Runtime9instance_E'}
# Assembly that C++ calls (not just takes the address of).
CALLED_FROM_CPP = ['art_quick_invoke_stub', 'art_quick_invoke_static_stub',
                   'art_quick_osr_stub', '__memcmp16', 'ExecuteSwitchImplAsm']
# Called from assembly through a function pointer, not by name.
CALLED_BY_POINTER = {'ExecuteSwitchImplCpp': 'art/runtime/interpreter/interpreter_switch_impl.h'}


def object_source(obj, build):
    """CMake object path -> source path.

    CMake names objects after the source path: relative to the source or
    build tree, or absolute (C:/x becomes C_/x on Windows)."""
    rel = os.path.relpath(obj, build).replace(os.sep, '/')
    rel = re.sub(r'^CMakeFiles/[^/]+\.dir/', '', rel)
    rel = re.sub(r'\.obj$', '', rel)
    m = re.match(r'^([A-Za-z])_/(.*)$', rel)
    candidates = [f'{m.group(1)}:/{m.group(2)}'] if m else []
    candidates += ['/' + rel, os.path.join(build, rel), os.path.join(os.getcwd(), rel)]
    for c in candidates:
        if os.path.isfile(c):
            return c
    raise SystemExit(f'cannot find the source of {obj}')


def annotated(text, name):
    rx = r'extern "C" ART_SYSV_ABI[^;{}()]*?\b' + re.escape(name) + r'\s*\('
    return re.search(rx, text) is not None


def all_annotated(text):
    """No unannotated extern "C" function in the file, other than
    declarations of the assembly's own art_quick_* stubs."""
    rx = r'extern "C"(?! ART_SYSV_ABI)(?!\s*\{)(?![^;{}()]*\bart_quick_[\w#]*\s*\()\s'
    return re.search(rx, text) is None


def main():
    build, aosp = sys.argv[1], sys.argv[2]
    objs = glob.glob(os.path.join(build, 'CMakeFiles', '*.dir', '**', '*.obj'), recursive=True)
    # Skip outputs that are not objects (asm_defines is compiled with -S).
    objs = [o for o in objs if open(o, 'rb').read(2) == b'\x64\x86']
    asm = [o for o in objs if o.endswith('.S.obj')]
    cpp = [o for o in objs if not o.endswith('.S.obj')]

    asm_defined, asm_undefined = set(), set()
    for o in asm:
        for name, defined in coff.symbols(o):
            (asm_defined if defined else asm_undefined).add(name)
    definers = {}
    for o in cpp:
        for name, defined in coff.symbols(o):
            if defined:
                definers.setdefault(name, o)

    problems, checked = [], 0
    for name in sorted(asm_undefined - asm_defined - DATA):
        if name.startswith('gesher_sysv_'):
            continue
        obj = definers.get(name)
        if obj is None:
            problems.append(f'{name}: called from assembly, defined nowhere in C++')
            continue
        src = object_source(obj, build)
        text = open(src, encoding='utf-8', errors='replace').read()
        # Names made by token pasting do not appear literally; then the whole
        # file must be annotated.
        if not (annotated(text, name) or all_annotated(text)):
            problems.append(f'{name}: defined in {src} without ART_SYSV_ABI')
        checked += 1

    for name in CALLED_FROM_CPP:
        hits = [f for f in glob.glob(os.path.join(aosp, 'art/runtime/**/*.[ch]*'), recursive=True)
                if 'arch/arm' not in f.replace(os.sep, '/') and 'arch/riscv' not in f.replace(os.sep, '/')
                and name in open(f, encoding='utf-8', errors='replace').read()
                and re.search(r'extern "C"[^;{}()]*?\b' + re.escape(name) + r'\s*\(',
                              open(f, encoding='utf-8', errors='replace').read())]
        bad = [f for f in hits if not annotated(open(f, encoding='utf-8', errors='replace').read(), name)]
        if not hits:
            problems.append(f'{name}: no extern "C" declaration found')
        problems += [f'{name}: declared in {f} without ART_SYSV_ABI' for f in bad]
        checked += 1

    for name, rel in CALLED_BY_POINTER.items():
        if not re.search(r'ART_SYSV_ABI void ' + name, open(os.path.join(aosp, rel)).read()):
            problems.append(f'{name}: {rel} lacks ART_SYSV_ABI')
        checked += 1

    print(f'{checked} assembly boundaries checked, {len(problems)} problem(s)')
    for p in problems[:40]:
        print('  ' + p)
    if len(problems) > 40:
        print(f'  ... and {len(problems) - 40} more')
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
