#!/usr/bin/env python3
"""Exercise actual exec selection bodies with declared host filesystem boundaries.

This never starts a process. Probe/module-path/encoding/Win32 comparison calls
are deterministic host replacements. Environment decoding and selection bodies
are copied verbatim from the selected source snapshot.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import tempfile

def main():
    ROOT = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT, help='Repository containing the production source')
    parser.add_argument('--output', type=Path, help='Retain generated source and logs in this directory')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--negative-control', action='store_true', help='Restore the observed empty-on-error behavior; the same checks must fail')
    args = parser.parse_args()
    temporary = tempfile.TemporaryDirectory(prefix='gesher-environment-lookup-') if args.output is None else None
    args.output = args.output or Path(temporary.name)
    args.output.mkdir(parents=True, exist_ok=True)
    source_path = args.source_root / 'compat/elf/exec/exec_spawn.cc'
    source = source_path.read_text()
    original_sha256 = hashlib.sha256(source.encode()).hexdigest()
    if args.negative_control:
        # Focused regression mutation, not a historical-source reconstruction.
        checked_decode = 'if (environment_path && !gesher::DecodeEnvironmentUtf8(environment_path, &path_value)) return EILSEQ;'
        checked_application = 'if (launch->application.empty()) return EILSEQ;'
        assert source.count(checked_decode) == 1
        assert source.count(checked_application) == 1
        source = source.replace(checked_decode, 'path_value = EnvironmentValue(environment_path);')
        source = source.replace(checked_application, '')
    first = source[source.index('bool IsSlash('):source.index('// What a file is,')]
    selection = source[source.index('bool IsShName('):source.index('\n}  // namespace\n\nstd::wstring Widen(')]
    public = source[source.index('std::wstring LauncherPath('):source.index('\nbool IsToolName(')]
    block = source[source.index('std::wstring EnvironmentBlock('):source.index('\nint ExitStatus(')]
    header = args.source_root / 'compat/posix/include/gesher/environment_utf16.h'
    reserved = args.source_root / 'compat/posix/include/gesher/reserved_names.h'
    preamble = r'''
    #include <algorithm>
    #include <cerrno>
    #include <cstdint>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <cwchar>
    #include <cwctype>
    #include <map>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <utility>
    #include <vector>
    #include "@HEADER@"
    #include "@RESERVED@"
    using DWORD = unsigned long;
    using UINT = unsigned;
    constexpr size_t MAX_PATH = 260;
    constexpr DWORD INVALID_FILE_ATTRIBUTES = static_cast<DWORD>(-1);
    constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 16;
    constexpr int TRUE = 1, CSTR_LESS_THAN = 1;
    static const char* process_path = ".";
    static const char* process_comspec = nullptr;
    static const char* process_launcher = nullptr;
    static int probes;
    static std::map<std::wstring, std::string> files;
    static DWORD GetFileAttributesW(const wchar_t* path) {
      return files.count(path) ? 0 : INVALID_FILE_ATTRIBUTES;
    }
    static UINT GetSystemDirectoryW(wchar_t* target, UINT capacity) {
      const wchar_t value[] = L"C:\\Windows\\System32";
      if (capacity > std::wcslen(value)) std::wcscpy(target, value);
      return static_cast<UINT>(std::wcslen(value));
    }
    static DWORD GetModuleFileNameW(void*, wchar_t* target, DWORD capacity) {
      const wchar_t value[] = L"C:\\fixture\\host.exe";
      if (capacity > std::wcslen(value)) std::wcscpy(target, value);
      return static_cast<DWORD>(std::wcslen(value));
    }
    static int _wcsicmp(const wchar_t* left, const wchar_t* right) {
      while (*left && *right && std::towlower(*left) == std::towlower(*right)) { ++left; ++right; }
      return static_cast<int>(std::towlower(*left)) - static_cast<int>(std::towlower(*right));
    }
    static int CompareStringOrdinal(const wchar_t* a, int as, const wchar_t* b, int bs, int) {
      std::wstring left(a, as), right(b, bs);
      for (auto& value : left) value = std::towlower(value);
      for (auto& value : right) value = std::towlower(value);
      return left < right ? 1 : left == right ? 2 : 3;
    }
    namespace gesher {
    char* GetWindowsEnvironment(const char* name) {
      if (std::strcmp(name, "PATH") == 0) return const_cast<char*>(process_path);
      if (std::strcmp(name, "ComSpec") == 0) return const_cast<char*>(process_comspec);
      if (std::strcmp(name, "GESHER_EXEC_LAUNCHER") == 0) return const_cast<char*>(process_launcher);
      return nullptr;
    }
    namespace exec {
    constexpr int kENOENT = 2, kE2BIG = 7, kENOEXEC = 8, kEACCES = 13, kEILSEQ = EILSEQ;
    struct Launch { std::wstring application, command_line; std::string kind; };
    std::wstring Widen(const std::string& input) { return std::wstring(input.begin(), input.end()); }
    std::string Narrow(const std::wstring& input) { return std::string(input.begin(), input.end()); }
    std::wstring WindowsPath(const std::string& input, const std::string& dir = "") {
      auto result = Widen(dir.empty() ? input : dir + "/" + input);
      std::replace(result.begin(), result.end(), L'/', L'\\');
      return result;
    }
    void AppendQuoted(std::wstring* command, const std::wstring& value) {
      *command += L'"'; *command += value; *command += L'"';
    }
    std::wstring LauncherPath();
    namespace {
    constexpr size_t kMaxCommandLine = 32767;
    int Probe(const std::wstring& path, std::string* header) {
      ++probes;
      auto found = files.find(path);
      if (found == files.end()) return kENOENT;
      *header = found->second;
      return 0;
    }
    '''.replace('@HEADER@', str(header)).replace('@RESERVED@', str(reserved))
    checks = r'''
    } } // namespace gesher::exec
    static int checks, failures;
    static void Check(const char* label, bool condition) {
      ++checks;
      std::printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
      if (!condition) ++failures;
    }
    static void Reset() {
      process_path = "."; process_comspec = nullptr; process_launcher = nullptr;
      probes = 0; errno = 0;
    }
    int main() {
      namespace ex = gesher::exec;
      files[L".\\victim.exe"] = "MZ";
      files[L"C:\\known\\good.exe"] = "MZ";
      files[L".\\fixture.cmd"] = "echo fixture";
      files[L"C:\\known\\text.sh"] = "echo fixture";
      char mutable_path[] = ".";
      const char malformed[] = { static_cast<char>(0xff), 0 };
      Reset(); process_path = mutable_path;
      mutable_path[0] = static_cast<char>(0xff);
      const auto explicit_child = ex::EnvironmentBlock({"CONTROL=clean"}, nullptr);
      ex::Launch launch;
      int result = ex::PrepareLaunch({"victim.exe"}, "", &launch);
      Check("clean replacement environment is valid", !explicit_child.empty());
      Check("bad borrowed parent PATH rejects bare lookup before current-directory probe", result == EILSEQ && probes == 0);
      Reset(); process_path = malformed;
      launch = {}; result = ex::PrepareLaunchProgram("victim.exe", {"victim.exe"}, "", true, &launch);
      Check("searching program API rejects bad parent PATH before probe", result == EILSEQ && probes == 0);
      Reset(); process_path = "";
      launch = {}; result = ex::PrepareLaunch({"victim.exe"}, "", &launch);
      Check("genuinely empty PATH retains current-directory lookup", result == 0 && launch.application == L".\\victim.exe");
      Reset(); process_path = malformed;
      launch = {}; result = ex::PrepareLaunch({"C:/known/good.exe"}, "", &launch);
      Check("direct PE path ignores unrelated malformed parent PATH", result == 0 && launch.application == L"C:\\known\\good.exe");
      Reset(); process_path = malformed;
      launch = {}; result = ex::PrepareLaunchProgram("C:/known/good.exe", {"alias"}, "", false, &launch);
      Check("nonsearching program API ignores malformed parent PATH", result == 0 && launch.application == L"C:\\known\\good.exe");
      Reset(); process_comspec = malformed;
      launch = {}; result = ex::PrepareLaunch({"fixture.cmd"}, "", &launch);
      Check("selected malformed ComSpec cannot produce success launch", result == EILSEQ);
      Reset(); process_launcher = malformed;
      launch = {}; result = ex::PrepareLaunch({"sh"}, "", &launch);
      Check("selected malformed launcher cannot produce success launch", result == EILSEQ);
      Reset(); process_path = malformed;
      launch = {}; result = ex::PrepareLaunch({"sh"}, "", &launch);
      Check("builtin shell ignores unrelated malformed parent PATH", result == 0 && launch.application == L"C:\\fixture\\gesher-exec.exe");
      Reset(); process_path = malformed;
      launch = {}; result = ex::PrepareLaunch({"C:/known/text.sh"}, "", &launch);
      Check("direct text file with valid launcher ignores parent PATH", result == 0 && launch.application == L"C:\\fixture\\gesher-exec.exe");
      Reset(); process_launcher = malformed; process_comspec = malformed;
      launch = {}; result = ex::PrepareLaunch({"C:/known/good.exe"}, "", &launch);
      Check("direct PE ignores unrelated malformed launcher and ComSpec", result == 0 && launch.application == L"C:\\known\\good.exe");
      std::printf("%d checks; %d failures (source-body host control only)\n", checks, failures);
      return failures ? 1 : 0;
    }
    '''
    generated = preamble + first + selection + '\n}\n' + public + '\n' + block + checks
    (args.output / 'exec_spawn_snapshot.cc').write_text(source)
    (args.output / 'check.cc').write_text(generated)
    command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(args.output / 'check.cc'), '-o', str(args.output / 'check')]
    (args.output / 'commands.json').write_text(json.dumps(command, indent=2) + '\n')
    subprocess.run(command, check=True, timeout=60)
    run = subprocess.run([str(args.output / 'check')], capture_output=True, text=True, timeout=10)
    (args.output / 'result.log').write_text(run.stdout + run.stderr)
    print('exec_spawn.cc original SHA256', original_sha256)
    print('compiled source SHA256', hashlib.sha256(source.encode()).hexdigest())
    if args.negative_control: print('Focused empty-on-error mutation; this is not historical source.')
    print(run.stdout + run.stderr, end='')
    if temporary is not None: temporary.cleanup()
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
