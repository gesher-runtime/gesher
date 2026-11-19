#!/usr/bin/env python3
"""Check selected-but-unavailable TLS with complete production ELF and shim.

A valid data-only AArch64 DSO passes through actual native TLS registration,
preparation, publication and address methods. Win32 primitives and ARM discovery
are explicit host boundaries. Any attempt to run guest instructions aborts.
This does not execute Windows, native ARM instructions or translated ARM code.
Requires a Clang compiler with LLD for the original AArch64 fixture.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import platform
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(repo: Path, out: Path, args) -> int:
    spec = importlib.util.spec_from_file_location('host', repo / 'tools/test_elf_atomic_load.py')
    host = importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
    elf, fixtures = host.snapshot(out, None, ('elf_loader_host_platform.cc',))
    provider = out / 'provider.c'
    provider.write_text('__thread int audit_tls = 37;\n__thread int getpid = 41;\n')
    version_map = out / 'provider.map'
    version_map.write_text('GV_TLS { global: audit_tls; getpid; local: *; };\n')
    library = out / 'libaudit_tls.so'
    clang = shlex.split(args.arm_cc)
    host.run(clang + ['--target=aarch64-linux-gnu', '-fuse-ld=lld', '-shared', '-fPIC', '-O2',
              '-fno-stack-protector', '-nostdlib', '-Wall', '-Wextra', '-Werror', '-Wl,--hash-style=both',
              '-Wl,-soname,libaudit_tls.so', '-Wl,--version-script=' + str(version_map),
              str(provider), '-o', str(library)], 'BUILD valid data-only AArch64 TLS provider')
    (out / 'readelf.txt').write_text(subprocess.check_output(['readelf', '-WlrsV', str(library)], text=True))
    client_source = out / 'client.c'
    client_source.write_text('int audit_anchor = 1;\n')
    client = Path(str(library) + '.client.so')
    host.run(clang + ['--target=aarch64-linux-gnu', '-fuse-ld=lld', '-shared', '-fPIC', '-O2',
                     '-fno-stack-protector', '-nostdlib', '-Wall', '-Wextra', '-Werror',
                     '-Wl,--hash-style=both', '-Wl,-soname,libaudit_client.so', str(client_source),
                     '-L' + str(out), '-Wl,--no-as-needed', '-l:libaudit_tls.so', '-o', str(client)],
             'BUILD valid data-only AArch64 dependency client')
    bridge = (repo / 'compat/arm64/dynarmic_bridge.cc').read_text()
    start = bridge.index('constexpr size_t kTcbSize =')
    tls = bridge[start:bridge.index('void* g_tlsdesc_static =', start)]
    boundary = out / 'arm_boundary.cc'
    boundary.write_text('''#include <gesher/arm64_runtime.h>
    #include <windows.h>
    #include <algorithm>
    #include <cstdint>
    #include <cstring>
    #include <cstdlib>
    #include <mutex>
    #include <vector>
    namespace gesher::arm64 {
    namespace {
    ''' + tls + '''
    }
    bool EnsureLoaded() { return true; }
    bool Available() { return true; }
    void AddCode(const void*, size_t, const char*) {}
    void RemoveCode(const void*, size_t) {}
    void* BindHostFunction(void*) { std::abort(); }
    void UnbindHostFunction(void*) { std::abort(); }
    void* HostEntry(const void*) { std::abort(); }
    uint64_t Call(const void*, const uint64_t*, size_t) { std::abort(); }
    int64_t TlsRegister(size_t size, size_t align) { return NativeTlsRegister(size, align); }
    bool TlsPrepare(int64_t off, const void* image, size_t init, size_t size) { return NativeTlsPrepare(off, image, init, size) != 0; }
    void TlsPublish(int64_t off, const void* image, size_t init, size_t size) { NativeTlsPublish(off, image, init, size); }
    bool TlsRollback(int64_t off, size_t size) { return NativeTlsRollback(off, size) != 0; }
    void* TlsAddress(int64_t off, size_t sym) { return NativeTlsAddress(off, sym); }
    void AddVaListFunction(const void*, int) { std::abort(); }
    bool IsGuestCode(const void*) { return false; }
    uint64_t RunProgram(const void*, const uint64_t*, size_t) { std::abort(); }
    void* GuestSymbol(const char*) { std::abort(); }
    void* TlsDescStatic() { std::abort(); }
    void* TlsDescUnresolvedWeak() { std::abort(); }
    uint64_t CallIfuncResolver(const void*) { std::abort(); }
    bool BoundaryPublished() { return g_tls_images.size() == 1 && g_tls_images.front().published && g_tls_images.front().total_size == 2 * sizeof(int); }
    }
    ''')
    shim = (repo / 'compat/bionic/shim.cc').read_text()
    state = re.search(r'thread_local std::string t_dlerror;\s*thread_local bool t_has_dlerror = false;', shim).group()
    start = shim.index('// dlsym: the shim\'s own symbols')
    dispatch = shim[start:shim.index('struct BionicDlInfo', start)]
    entries = []
    for name in ('dlsym', 'dlvsym', 'dlerror', 'dlclose'):
        values = re.findall(r'^[ \t]*ENTRY\("' + name + r'",[^\n]+;', shim, re.MULTILINE)
        assert len(values) == 1
        entries.append(values[0])
    driver = out / 'driver.cc'
    driver.write_text(r'''#include <gesher/elf_loader.h>
    #include <cstdio>
    #include <cstring>
    #include <string>
    #include <unordered_map>
    #define BIONIC_SYSV __attribute__((sysv_abi))
    namespace gesher::arm64 { bool BoundaryPublished(); }
    namespace gesher::bionic {
    namespace {
    bool IsPlatformLibraryHandle(const void*) { return false; }
    int builtin_lookups;
    int BIONIC_SYSV BuiltinGetpid() { return 719; }
    void* LookupBionicSymbol(const char* name) {
      ++builtin_lookups;
      return std::strcmp(name, "getpid") == 0 ? reinterpret_cast<void*>(&BuiltinGetpid) : nullptr;
    }
    bool TraceDl() { return false; }
    ''' + state + '\n' + dispatch + r'''
    } }
    int main(int argc, char** argv) {
      if (argc != 2) return 2;
      using namespace gesher::bionic;
      std::unordered_map<std::string, void*> table;
    #define ENTRY(name, value) table.emplace(name, reinterpret_cast<void*>(value))
    ''' + '\n'.join(entries) + r'''
    #undef ENTRY
      auto lookup = reinterpret_cast<void*(BIONIC_SYSV*)(void*, const char*, const char*)>(table.at("dlvsym"));
      auto ordinary = reinterpret_cast<void*(BIONIC_SYSV*)(void*, const char*)>(table.at("dlsym"));
      auto error = reinterpret_cast<const char*(BIONIC_SYSV*)()>(table.at("dlerror"));
      gesher::elf::LoadOptions options;
      options.allow_shimmed_needed = false;
      options.stub_unresolved_functions = false;
      std::string message;
      auto library = gesher::elf::Load(argv[1], &message, options);
      if (!library) { std::printf("LOAD FAIL %s\n", message.c_str()); return 2; }
    auto client = gesher::elf::Load((std::string(argv[1]) + ".client.so").c_str(), &message, options);
    if (!client) { std::printf("CLIENT LOAD FAIL %s\n", message.c_str()); return 2; }
      int failures = 0, checks = 0;
      auto check = [&](bool ok, const char* label) {
        ++checks; if (!ok) ++failures;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
      };
      check(gesher::elf::IsAarch64(library), "real linked provider has AArch64 identity");
      check(gesher::arm64::BoundaryPublished(), "actual native TLS registry published the valid module");
      for (void* handle : {static_cast<void*>(library), static_cast<void*>(client), static_cast<void*>(nullptr)}) {
        for (const char* name : {"audit_tls", "getpid"}) {
          check(error() == nullptr, "initial error state is clear");
          gesher::elf::LookupStatus status = gesher::elf::LookupStatus::kResolved;
          void* address = handle
              ? gesher::elf::LookupVersion(static_cast<gesher::elf::Library*>(handle), name, "GV_TLS", &status)
              : gesher::elf::LookupGlobalVersion(name, "GV_TLS", &status);
          check(address == nullptr && status == gesher::elf::LookupStatus::kUnavailable,
                "real native TLS storage failure is distinct from absence and resolved null");
          const int before = builtin_lookups;
          void* result = lookup(handle, name, "GV_TLS");
          const char* detail = error();
          check(result == nullptr, "unavailable native TLS produces no invented address");
          check(detail && std::strstr(detail, "unavailable") && std::strstr(detail, name),
                "unavailable native TLS is reported as its own failure");
          check(builtin_lookups == before, "selected unavailable TLS cannot use a same-name builtin");
          check(error() == nullptr, "unavailable TLS error is consumed once");
          result = ordinary(handle, name);
          detail = error();
          check(result == nullptr, "ordinary unavailable TLS produces no address");
          check(detail && std::strstr(detail, "unavailable"), "ordinary unavailable TLS is reported as failure");
          check(builtin_lookups == before, "ordinary unavailable TLS cannot use a builtin fallback");
          status = gesher::elf::LookupStatus::kResolved;
          address = handle
              ? gesher::elf::LookupVersion(static_cast<gesher::elf::Library*>(handle), name, "GV_MISSING", &status)
              : gesher::elf::LookupGlobalVersion(name, "GV_MISSING", &status);
          check(address == nullptr && status == gesher::elf::LookupStatus::kNotFound,
                "an actually absent named version remains distinct from unavailable storage");
        }
        check(lookup(handle, "audit_tls", "GV_MISSING") == nullptr && error() != nullptr,
              "absent named version remains a real shim failure");
      }
      std::printf("UNAVAILABLE_TLS_CONTRACT checks=%d failures=%d\n", checks, failures);
      return failures ? 1 : 0;
    }
    ''')
    inputs = [elf / name for name in host.PRODUCTION_UNITS]
    inputs += [fixtures / 'elf_loader_host_platform.cc', boundary, driver]
    includes = ['-I', str(fixtures / 'elf_loader_host_platform'), '-I', str(elf / 'include'), '-I', str(elf),
                '-I', str(fixtures / 'arm64-include')]
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-pthread']
    link_flags = ['-pthread']
    if args.sanitize:
        sanitizers = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        flags += sanitizers + ['-fno-pie']
        link_flags += sanitizers + ['-no-pie']
    compiler = shlex.split(args.cxx)
    objects = []
    for i, source in enumerate(inputs):
        obj = out / f'{i}.o'
        host.run(compiler + flags + includes + ['-c', str(source), '-o', str(obj)], 'COMPILE ' + source.name)
        objects.append(str(obj))
    binary = out / 'probe'
    host.run(compiler + objects + link_flags + ['-o', str(binary)], 'LINK actual ELF/native TLS/shim availability probe')
    manifest = {str(source): hashlib.sha256(source.read_bytes()).hexdigest() for source in inputs}
    for name, value in [('complete_shim', shim), ('complete_native_bridge', bridge), ('actual_native_tls', tls), ('actual_shim', state + dispatch + '\n'.join(entries))]:
        manifest[name] = hashlib.sha256(value.encode()).hexdigest()
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('SOURCE_MANIFEST ' + json.dumps(manifest, sort_keys=True), flush=True)
    env = os.environ.copy()
    if args.sanitize:
        previous = env.get('ASAN_OPTIONS', '')
        env['ASAN_OPTIONS'] = previous + (':' if previous else '') + 'detect_leaks=0'
        print('SANITIZERS address,undefined; fatal findings; detect_leaks=0 (ptrace)', flush=True)
    result = subprocess.run([str(binary), str(library)], env=env, timeout=30)
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--arm-cc', default=os.environ.get('ARM_CC', 'clang'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        parser.error('the explicit host boundary requires Linux x86-64')
    print('BOUNDARY complete ELF, actual native TLS methods and actual shim; no guest execution', flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-unavailable-tls-') as temporary:
        return run(ROOT, Path(temporary), args)


if __name__ == '__main__':
    raise SystemExit(main())
