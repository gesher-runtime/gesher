// gesher-exec: runs an Android x86-64 ELF executable as a Windows process
// (docs/exec.md, docs/decisions/0035-exec-elf-processes.md).
//
//   gesher-exec --elf <program> <argv0> [args...]
//   gesher-exec --sh [-c <command> | <script> [args...]]
//
// --elf loads <program> (a PIE: ET_DYN with PT_INTERP, as the NDK builds
// executables) with Gesher's ELF loader and the bionic shim as the main
// program, the way bionic's linker would: its DT_NEEDED libraries from
// $LD_LIBRARY_PATH (then the program's own directory), init functions called
// with (argc, argv, envp), then its entry point (_start) on a stack laid out as
// Linux's: argc, argv, envp, the auxiliary vector. bionic's __libc_init, exit,
// _exit and getauxval are provided here; stdin/stdout/stderr are the
// inherited handles (binary). The exit status is the program's (low 8 bits),
// 128 + signal when it aborts or faults, 1 when it cannot be linked (bionic's
// "CANNOT LINK EXECUTABLE").
//
// --sh runs gesher_shell.cc (docs/exec.md, ADR 0060).
//
// libcore's process natives (compat/libcore/process.cc) start this for
// ProcessBuilder / Runtime.exec of an ELF file or of sh.

#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>
#include <gesher/system_properties.h>

#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include "exec_spawn.h"
#include "gesher_shell.h"

#define SYSV __attribute__((sysv_abi))

namespace elf = gesher::elf;
namespace ex = gesher::exec;

// Enters the program: %rsp = `sp` (argc at 0(%rsp)), %rdx = 0 (no rtld fini
// function), jump to `entry`. Microsoft ABI: rcx = entry, rdx = sp.
extern "C" [[noreturn]] void gesher_exec_enter(void* entry, void* sp);
asm(".text\n"
    ".globl gesher_exec_enter\n"
    ".p2align 4\n"
    "gesher_exec_enter:\n"
    "  movq %rdx, %rsp\n"
    "  xorl %ebp, %ebp\n"
    "  xorl %edx, %edx\n"
    "  jmpq *%rcx\n");

namespace {

// Linux's signal numbers, for exit statuses.
constexpr int kSigill = 4, kSigtrap = 5, kSigabrt = 6, kSigbus = 7, kSigfpe = 8, kSigsegv = 11;
// The CRT's SIGABRT (gesher-base's <signal.h> uses Linux's numbers).
constexpr int kCrtSigabrt = 22;

const char* g_name = "gesher-exec";  // for messages: the program's argv[0]

[[noreturn]] void Die(int status, const char* fmt, const char* a = "", const char* b = "") {
  fprintf(stderr, fmt, a, b);
  fflush(stderr);
  ExitProcess(static_cast<UINT>(status));
}

// --- the program's view of its process -------------------------------------

struct ProgramState {
  std::wstring path;                 // the ELF file
  std::vector<std::string> args;     // argv
  std::vector<std::string> env;      // "NAME=value"
  std::vector<char*> argv, envp;     // NULL-terminated
  std::vector<uint64_t> auxv;        // pairs, AT_NULL-terminated
  alignas(16) uint8_t random[16] = {};
};
ProgramState* g_program = nullptr;

// bionic's data the program may import: environ and __progname.
char** g_environ = nullptr;
const char* g_progname = "";

std::atomic<bool> g_exiting{false};

using FinalizeFn = void (SYSV*)(void*);
using AtexitFn = int (SYSV*)(void (SYSV*)(void*), void*, void*);
using GetauxvalFn = uint64_t (SYSV*)(uint64_t);
GetauxvalFn g_shim_getauxval = nullptr;

// exit(): __cxa_atexit/atexit handlers (static destructors, the executable's
// .fini_array) in reverse order, stdio flushed, then the low 8 bits as the
// process's exit code, as on Linux.
[[noreturn]] void SYSV Exit(int status) {
  gesher::bionic::ExitIfVforkChild(status);  // a fork child exiting without exec
  if (!g_exiting.exchange(true)) {
    if (auto finalize = reinterpret_cast<FinalizeFn>(gesher::bionic::LookupBionicSymbol("__cxa_finalize"))) {
      finalize(nullptr);
    }
  }
  fflush(nullptr);
  ExitProcess(static_cast<UINT>(status) & 0xff);
}

// _exit()/_Exit(): no handlers and no stdio flush, as on Linux.
[[noreturn]] void SYSV UnderscoreExit(int status) {
  gesher::bionic::ExitIfVforkChild(status);
  ExitProcess(static_cast<UINT>(status) & 0xff);
}

uint64_t SYSV Getauxval(uint64_t type) {
  if (g_program != nullptr && type != 0) {
    for (size_t i = 0; i + 1 < g_program->auxv.size(); i += 2) {
      if (g_program->auxv[i] == type) return g_program->auxv[i + 1];
    }
  }
  return g_shim_getauxval != nullptr ? g_shim_getauxval(type) : 0;
}

// bionic's __libc_fini: the executable's .fini_array between crtbegin's -1
// and crtend's NULL, run in reverse.
void SYSV LibcFini(void* array) {
  using Dtor = void (SYSV*)();
  auto* fini = static_cast<Dtor*>(array);
  const Dtor minus1 = reinterpret_cast<Dtor>(static_cast<uintptr_t>(-1));
  if (fini == nullptr || fini[0] != minus1) return;
  ++fini;
  size_t count = 0;
  while (fini[count] != nullptr) ++count;
  while (count > 0) {
    Dtor d = fini[--count];
    if (d != minus1) d();
  }
}

// bionic's structors_array_t (libc_init_common.h): the dynamic __libc_init
// uses fini_array only.
struct Structors {
  void* preinit_array;
  void* init_array;
  void* fini_array;
};
using MainFn = int (SYSV*)(int, char**, char**);

// __libc_init(raw_args, onexit, main, structors): what the NDK's _start calls
// (crtbegin.c's _start_main). The dynamic linker already ran the init arrays.
[[noreturn]] void SYSV LibcInit(void* raw_args, void (SYSV*)(void), MainFn slingshot,
                                const Structors* structors) {
  auto* words = static_cast<uintptr_t*>(raw_args);
  int argc = static_cast<int>(words[0]);
  char** argv = reinterpret_cast<char**>(words + 1);
  char** envp = argv + argc + 1;
  g_environ = envp;
  if (structors != nullptr && structors->fini_array != nullptr) {
    if (auto atexit_fn = reinterpret_cast<AtexitFn>(gesher::bionic::LookupBionicSymbol("__cxa_atexit"))) {
      atexit_fn(LibcFini, structors->fini_array, nullptr);
    }
  }
  Exit(slingshot(argc, argv, envp));
}

// A fault or exception nothing handled: gesher-base's crash report first (the
// filter installed before this one), then 128 + the signal Linux would send.
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
  if (g_previous_filter != nullptr) g_previous_filter(info);
  int status = ex::ExitStatus(info->ExceptionRecord->ExceptionCode);
  if (status < 128 || status > 128 + 64) status = 128 + kSigsegv;
  const char* what = status == 128 + kSigsegv ? "SIGSEGV"
                     : status == 128 + kSigill ? "SIGILL"
                     : status == 128 + kSigfpe ? "SIGFPE"
                     : status == 128 + kSigtrap ? "SIGTRAP"
                     : status == 128 + kSigbus ? "SIGBUS"
                                               : "SIGABRT";
  fprintf(stderr, "%s: terminated by %s (exception 0x%08lx)\n", g_name, what,
          info->ExceptionRecord->ExceptionCode);
  fflush(stdout);
  fflush(stderr);
  ExitProcess(static_cast<UINT>(status));
}

// The same as a vectored handler, last in line: faults in the program's code
// (no Windows unwind data) are taken here before Windows walks its stack. The
// loader's and gesher-base's handlers (unresolved imports, %fs sites, stack
// growth) come first.
LONG CALLBACK OnFatal(EXCEPTION_POINTERS* info) {
  switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return OnUnhandled(info);
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
}

// abort() (the shim's runs the program's SIGABRT handler first, then the
// CRT's abort, which raises the CRT's SIGABRT): 128 + SIGABRT.
void OnCrtAbort(int) {
  fflush(stdout);
  fflush(stderr);
  ExitProcess(128 + kSigabrt);
}

// LD_LIBRARY_PATH's directories (':' or ';' separated; "C:/x" kept whole).
std::vector<std::string> LibraryPath(const std::string& value) {
  std::vector<std::string> out;
  char sep = value.find(';') != std::string::npos ? ';' : ':';
  size_t start = 0;
  while (start < value.size()) {
    size_t end = value.find(sep, start);
    if (end == std::string::npos) end = value.size();
    if (sep == ':' && end - start == 1 && end + 1 < value.size() && (value[end + 1] == '/' || value[end + 1] == '\\')) {
      end = value.find(sep, end + 1);
      if (end == std::string::npos) end = value.size();
    }
    if (end > start) out.push_back(value.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

DWORD WINAPI ProgramThread(void*) {
  ProgramState& p = *g_program;
  if (!elf::SetupThreadTls()) Die(1, "%s: no TLS slot for the program's thread\n", g_name);

  gesher::bionic::InstallBionicShim();
  // The program is the whole process, so the shim owns every signal: a signal
  // another process sends reaches the program's handlers, and SIG_DFL acts as
  // Linux's default (docs/exec.md).
  gesher::bionic::InstallSignalHook(true);
  g_shim_getauxval = reinterpret_cast<GetauxvalFn>(gesher::bionic::LookupBionicSymbol("getauxval"));
  gesher::bionic::AddBionicSymbol("__libc_init", reinterpret_cast<void*>(&LibcInit));
  gesher::bionic::AddBionicSymbol("exit", reinterpret_cast<void*>(&Exit));
  gesher::bionic::AddBionicSymbol("_exit", reinterpret_cast<void*>(&UnderscoreExit));
  gesher::bionic::AddBionicSymbol("_Exit", reinterpret_cast<void*>(&UnderscoreExit));
  gesher::bionic::AddBionicSymbol("getauxval", reinterpret_cast<void*>(&Getauxval));
  gesher::bionic::AddBionicSymbol("environ", &g_environ);
  gesher::bionic::AddBionicSymbol("__progname", &g_progname);

  int argc = static_cast<int>(p.args.size());
  g_environ = p.envp.data();
  elf::SetInitArguments(argc, p.argv.data(), p.envp.data());

  elf::LoadOptions options;
  options.main_executable = true;
  for (const std::string& e : p.env) {
    if (e.rfind("LD_LIBRARY_PATH=", 0) == 0) options.search_paths = LibraryPath(e.substr(16));
  }
  std::string program = ex::Narrow(p.path);
  size_t slash = program.find_last_of("\\/");
  if (slash != std::string::npos) options.search_paths.push_back(program.substr(0, slash));

  std::string error;
  elf::Library* lib = elf::Load(program.c_str(), &error, options);
  if (lib == nullptr) Die(1, "CANNOT LINK EXECUTABLE \"%s\": %s\n", g_name, error.c_str());
  elf::ExecutableInfo info;
  if (!elf::GetExecutableInfo(lib, &info) || info.entry == nullptr) {
    Die(1, "CANNOT LINK EXECUTABLE \"%s\": %s has no entry point\n", g_name, program.c_str());
  }
  if (!info.has_interp) {
    fprintf(stderr, "%s: warning: %s has no PT_INTERP (a shared library?); entering it anyway\n", g_name,
            program.c_str());
  }

  // The auxiliary vector (getauxval answers from it too).
  BCryptGenRandom(nullptr, p.random, sizeof(p.random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  const uint64_t auxv[] = {
      3, reinterpret_cast<uint64_t>(info.phdr),      // AT_PHDR
      4, 56,                                          // AT_PHENT: sizeof(Elf64_Phdr)
      5, info.phnum,                                  // AT_PHNUM
      6, 4096,                                        // AT_PAGESZ
      7, 0,                                           // AT_BASE: no interpreter mapped
      8, 0,                                           // AT_FLAGS
      9, reinterpret_cast<uint64_t>(info.entry),     // AT_ENTRY
      11, 10000, 12, 10000, 13, 10000, 14, 10000,    // AT_UID, AT_EUID, AT_GID, AT_EGID
      16, 0,                                          // AT_HWCAP
      17, 100,                                        // AT_CLKTCK
      23, 0,                                          // AT_SECURE
      25, reinterpret_cast<uint64_t>(p.random),      // AT_RANDOM
      26, 0,                                          // AT_HWCAP2
      31, reinterpret_cast<uint64_t>(p.argv[0]),     // AT_EXECFN
      0, 0};                                          // AT_NULL
  p.auxv.assign(std::begin(auxv), std::end(auxv));
  // getuid() and the like answer for the shim; ask it for consistency.
  if (g_shim_getauxval != nullptr) {
    for (size_t i = 0; i + 1 < p.auxv.size(); i += 2) {
      if (p.auxv[i] >= 11 && p.auxv[i] <= 14) p.auxv[i + 1] = g_shim_getauxval(p.auxv[i]);
    }
  }

  // The initial stack: argc, argv[], NULL, envp[], NULL, auxv pairs.
  size_t words = 1 + p.argv.size() + p.envp.size() + p.auxv.size();
  auto* raw = static_cast<uint8_t*>(alloca(words * 8 + 32));
  auto* block = reinterpret_cast<uint64_t*>((reinterpret_cast<uintptr_t>(raw) + 15) & ~uintptr_t{15});
  size_t w = 0;
  block[w++] = static_cast<uint64_t>(argc);
  for (char* a : p.argv) block[w++] = reinterpret_cast<uint64_t>(a);
  for (char* e : p.envp) block[w++] = reinterpret_cast<uint64_t>(e);
  for (uint64_t a : p.auxv) block[w++] = a;
  fflush(nullptr);
  if (info.aarch64) {
    // An arm64-v8a program: its _start runs through the translator with this
    // block as its stack (docs/arm64.md); it ends with exit() like any other.
    gesher::arm64::RunProgram(info.entry, block, words);
    Die(1, "%s: the program's _start returned\n", g_name);
  }
  gesher_exec_enter(info.entry, block);
}

int RunElf(const std::wstring& path, const std::vector<std::string>& args) {
  static ProgramState state;
  g_program = &state;
  state.path = path;
  state.args = args;
  // The environment, as UTF-8 "NAME=value" (Windows' per-drive "=C:" entries
  // are left out).
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* e = block; *e != 0; e += wcslen(e) + 1) {
      if (*e != L'=') state.env.push_back(ex::Narrow(e));
    }
    FreeEnvironmentStringsW(block);
  }
  for (std::string& a : state.args) state.argv.push_back(a.data());
  state.argv.push_back(nullptr);
  for (std::string& e : state.env) state.envp.push_back(e.data());
  state.envp.push_back(nullptr);
  g_name = state.args.empty() ? "gesher-exec" : state.args[0].c_str();
  const char* base = strrchr(g_name, '/');
  g_progname = base != nullptr ? base + 1 : g_name;

  // Linux's default 8 MiB stack, committed: NDK code does not probe the stack
  // as Windows code does, so it may skip the guard page.
  HANDLE thread = CreateThread(nullptr, 8u << 20, ProgramThread, nullptr, 0, nullptr);
  if (thread == nullptr) Die(1, "%s: cannot create the program's thread\n", g_name);
  WaitForSingleObject(thread, INFINITE);
  // _start never returns; the program ends with exit().
  return 1;
}

}  // namespace

int main() {
  // Publish the inherited property session before the shell copies its env.
  gesher::properties::Initialize();
  // Bytes in and out unchanged, as on Linux; no Windows error dialogs.
  _setmode(0, _O_BINARY);
  _setmode(1, _O_BINARY);
  _setmode(2, _O_BINARY);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  signal(kCrtSigabrt, OnCrtAbort);
  g_previous_filter = SetUnhandledExceptionFilter(OnUnhandled);
  AddVectoredExceptionHandler(0, OnFatal);

  int argc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(ex::Narrow(wargv[i]));
  std::wstring elf_path = argc >= 3 ? wargv[2] : L"";
  LocalFree(wargv);

  if (args.size() >= 3 && args[0] == "--elf") {
    return RunElf(elf_path, std::vector<std::string>(args.begin() + 2, args.end()));
  }
  if (!args.empty() && args[0] == "--sh") {
    return ex::RunShell(std::vector<std::string>(args.begin() + 1, args.end()));
  }
  if (args.size() >= 2 && args[0] == "--tool") {
    return ex::RunTool(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
  }
  fprintf(stderr,
          "usage: gesher-exec --elf <program> <argv0> [args...]\n"
          "       gesher-exec --sh [-c <command> | <script> [args...]]\n"
          "       gesher-exec --tool <name> [args...]\n");
  return 2;
}
