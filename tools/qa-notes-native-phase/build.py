#!/usr/bin/env python3
"""Build original and observation-only bridges, using the product's pinned ABI."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request

import overlay

ROOT = Path(__file__).resolve().parents[2]
JNI_COMMIT = '2d584ec2da2c5271ed1576593c6590d652806f0d'
JNI_RAW_SHA = '2a3cea5e4306872202592406593f6465f07b848d75ed03ad07e2936479867f0a'
JNI_PATCHED_SHA = '6bf820c7e2d01c0eb05e7907ccaeb15f0efdeef3d3ec99bb380c829b7a84448d'
JNI_URL = ('https://android.googlesource.com/platform/libnativehelper/+/' + JNI_COMMIT +
           '/include_jni/jni.h?format=TEXT')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def prepare_jni(out):
    root = out / 'jni-source'
    root.mkdir()
    path = root / 'include_jni/jni.h'
    path.parent.mkdir()
    supplied = os.environ.get('QA_NOTES_PRISTINE_JNI')
    if supplied:
        raw = Path(supplied).read_bytes()
    else:
        with urllib.request.urlopen(JNI_URL, timeout=60) as response:
            raw = base64.b64decode(response.read(), validate=True)
    if hashlib.sha256(raw).hexdigest() != JNI_RAW_SHA:
        raise ValueError('Pinned real ART JNI source differs')
    path.write_bytes(raw)
    for name in ('0001-no-dllexport-in-gesher.patch', '0002-sysv-jni-in-gesher.patch'):
        subprocess.run(['git', 'apply', '--include=include_jni/jni.h',
                        str(ROOT / 'patches/libnativehelper' / name)], cwd=root, check=True)
    if sha(path) != JNI_PATCHED_SHA:
        raise ValueError('Production JNI calling convention patches differ')
    return root

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--cmake', default='cmake')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    import diagnostic
    proof = diagnostic.source_proof()
    jni = prepare_jni(out)
    package = out / 'package'
    package.mkdir()
    commands = []
    compiled = {}
    overlays = {}
    llvm = Path(os.environ['LLVM_MINGW']).resolve()
    compiler = llvm / 'bin/x86_64-w64-mingw32-clang++'
    version = subprocess.check_output([str(compiler), '--version'], text=True)
    if 'clang version 23.1.3' not in version:
        raise ValueError('Pinned llvm-mingw compiler version changed')
    for mode in ('rebuilt', 'trace'):
        work = out / mode
        work.mkdir()
        source = work / 'source'
        shutil.copytree(ROOT / 'compat/webview', source / 'compat/webview')
        shutil.copyfile(ROOT / 'tools/qa-notes-native-phase/phase_trace.h', source / 'compat/webview/phase_trace.h')
        if mode == 'trace':
            for relative in (overlay.HOST, overlay.JNI):
                path = source / relative
                original = path.read_text()
                value, additions = overlay.apply(relative, original)
                path.write_text(value)
                overlays[relative] = dict(original_sha256=hashlib.sha256(original.encode()).hexdigest(),
                                         overlay_sha256=sha(path), insertions=additions)
        cmake = r'''cmake_minimum_required(VERSION 3.24)
project(notes_phase_bridge LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_BUILD_TYPE RelWithDebInfo)
include("@ROOT@/cmake/webview2_sdk.cmake")
add_library(gesher_webview_bridge SHARED
  source/compat/webview/webview_host.cc source/compat/webview/webview2_loader.cc
  source/compat/webview/webview_jni.cc source/compat/webview/msvc_shims.cc
  source/compat/webview/webview_bridge.def)
target_include_directories(gesher_webview_bridge PRIVATE source/compat/webview "@JNI@/include_jni")
target_include_directories(gesher_webview_bridge SYSTEM PRIVATE ${WEBVIEW2_INCLUDE_DIRS})
target_compile_definitions(gesher_webview_bridge PRIVATE
  GESHER USE_MINGW WIN32_LEAN_AND_MEAN NOMINMAX NOGDI _USE_MATH_DEFINES
  __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64 _POSIX_THREAD_SAFE_FUNCTIONS
  GESHER_WEBVIEW_SHARED GESHER_WEBVIEW2_STATIC)
target_compile_options(gesher_webview_bridge PRIVATE -mno-ms-bitfields -fno-exceptions
  -fsized-deallocation -Wall -Werror -Wno-unused-parameter)
set_source_files_properties(source/compat/webview/msvc_shims.cc PROPERTIES COMPILE_OPTIONS -fno-emulated-tls)
target_link_libraries(gesher_webview_bridge PRIVATE ole32 oleaut32 uuid shlwapi urlmon
  user32 gdi32 advapi32 bcrypt ${WEBVIEW2_STATIC_LIB} version)
target_link_options(gesher_webview_bridge PRIVATE -static -s -Wl,--exclude-all-symbols)
set_target_properties(gesher_webview_bridge PROPERTIES PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "gesher_webview_bridge")
'''.replace('@ROOT@', ROOT.as_posix()).replace('@JNI@', jni.as_posix())
        (work / 'CMakeLists.txt').write_text(cmake)
        command = [a.cmake, '-S', str(work), '-B', str(work / 'build'), '-G', 'Ninja',
                   '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/toolchain-llvm-mingw.cmake'),
                   '-DLLVM_MINGW=' + str(llvm), '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
        commands.append(command)
        subprocess.run(command, check=True)
        command = [a.cmake, '--build', str(work / 'build'), '--parallel', '4']
        commands.append(command)
        subprocess.run(command, check=True)
        dll = work / 'build/libgesher_webview_bridge.so'
        if dll.read_bytes()[:2] != b'MZ':
            raise ValueError('Produced bridge is not a Windows PE')
        shutil.copyfile(dll, package / (mode + '-libgesher_webview_bridge.so'))
        compiled[mode] = json.loads((work / 'build/compile_commands.json').read_text())
        sdk = work / 'build/microsoft.web.webview2.1.0.4258.31.nupkg'
        if sha(sdk) != '56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9':
            raise ValueError('Product WebView2 SDK digest differs')
    receipt = dict(source=proof, run=os.environ.get('GITHUB_RUN_ID'),
                   attempt=os.environ.get('GITHUB_RUN_ATTEMPT'),
                   producer_job=os.environ.get('GITHUB_JOB'),
                   compiler=dict(path=str(compiler), sha256=sha(compiler), version=version),
                   jni=dict(commit=JNI_COMMIT, url=JNI_URL, original_sha256=JNI_RAW_SHA,
                            patched_sha256=JNI_PATCHED_SHA), sdk_sha256=sha(sdk),
                   commands=commands, compile_commands=compiled, overlays=overlays,
                   outputs={p.name: sha(p) for p in sorted(package.iterdir())})
    (package / 'producer.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(dict(outputs=receipt['outputs'], source=proof['diagnostic_head']), indent=2))

if __name__ == '__main__':
    main()
