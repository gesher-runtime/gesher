// gesher: starting programs the way Linux's execvp would, on Windows
// (docs/exec.md, docs/decisions/0035-exec-elf-processes.md).
//
// Shared by libcore's process natives in dalvikvm (compat/libcore/process.cc:
// ProcessBuilder, Runtime.exec) and by the launcher's small shell
// (compat/elf/exec/gesher_exec.cc). A program is found as execvp finds it
// (a name with a '/' is a path, relative to the new working directory; a bare
// name is looked up in PATH), its kind decides how it runs:
//
//   - an ELF x86-64 executable (an Android PIE, often lib*.so in an app's
//     nativeLibraryDir) runs in a new gesher-exec.exe process, which loads it
//     with the ELF loader and the bionic shim as its main program;
//   - a PE image (.exe) runs as itself, a .cmd/.bat through cmd.exe;
//   - "sh", /system/bin/sh, /bin/sh and "#!...sh" scripts (and text files
//     without "#!", as Linux's execvp does) run in gesher-exec's shell; other
//     "#!" interpreters are looked up like programs;
//   - Android's /system/bin programs gesher-exec provides (IsToolName: ls,
//     cat, id, ps, getprop, setprop, logcat, pm, kill, sleep), as a bare name
//     or a /system/bin path, run as `gesher-exec --tool <name>`;
//   - a static executable (ET_EXEC) is ENOEXEC (ADR 0055).
//
// Paths are UTF-8; a Linux path without a drive is on the current drive, as
// gesher-base maps it.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gesher::exec {

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& utf16);

// Linux errno values the functions below return (gesher-base's numbering).
constexpr int kENOENT = 2;
constexpr int kE2BIG = 7;
constexpr int kENOEXEC = 8;
constexpr int kENOMEM = 12;
constexpr int kEACCES = 13;
constexpr int kENOTDIR = 20;

// strerror for the values above (and a generic text for others).
const char* ErrorText(int error);

// The errno closest to a Win32 error code.
int ErrnoFromWin32(DWORD error);

// Appends `arg` to a command line, quoted for CommandLineToArgvW and the CRT.
void AppendQuoted(std::wstring* command_line, const std::wstring& arg);

// The full Windows path of `path`: relative paths are taken against `dir`
// (when not empty) or the current directory; '/' becomes '\'.
std::wstring WindowsPath(const std::string& path, const std::string& dir = "");

// gesher-exec.exe: $GESHER_EXEC_LAUNCHER, else next to the running executable.
std::wstring LauncherPath();

// How a program runs.
struct Launch {
  std::wstring application;   // the image CreateProcessW starts
  std::wstring command_line;  // its full command line
  std::string kind;           // "elf", "exe", "batch", "sh", "script" (for logs)
};

// Finds `argv[0]` as execvp would (PATH: the calling process's, as the JDK
// does), with relative paths against `dir`, and fills `launch`. Returns 0, or
// a Linux errno: ENOENT (not found), EACCES (a directory, or not readable),
// ENOEXEC (not a format Gesher runs), E2BIG (command line too long).
int PrepareLaunch(const std::vector<std::string>& argv, const std::string& dir, Launch* launch);

// The same with `path` as PATH's value (the shell's own PATH variable).
int PrepareLaunchInPath(const std::vector<std::string>& argv, const std::string& dir, const std::string& path,
                        Launch* launch);

// The Windows file a bare name would run from `path` (as PrepareLaunchInPath
// finds it), or "" if there is none. Tools and sh are not files: "".
std::wstring FindInPath(const std::string& name, const std::string& path);

// The same for exec*(path, argv): `program` is the file to run (found in PATH
// when `search_path` and it has no '/', as execvp; else a path, as execv),
// `argv` what it gets, argv[0] included.
int PrepareLaunchProgram(const std::string& program, const std::vector<std::string>& argv,
                         const std::string& dir, bool search_path, Launch* launch);

// Whether `name` (a bare name or /system/bin/<name>, /system/xbin/<name>) is
// one of the Android programs gesher-exec provides itself (`--tool`):
// getprop, setprop, logcat, ls, cat, id, ps, pm, kill, sleep, toybox.
bool IsToolName(const std::string& name);

// gesher-exec --tool: runs the built-in program `name` with `args` (argv[1..]).
// 127 if there is no such tool. Defined in gesher_tools.cc.
int RunTool(const std::string& name, const std::vector<std::string>& args);

// One file descriptor a child gets: its number and the handle behind it.
struct ChildFd {
  int fd;
  HANDLE handle;
};

// CreateProcessW for `launch` with exactly `fds` as the child's file
// descriptors: 0-2 are its standard handles, and every fd also goes into the
// C runtime's inherited-fd block (STARTUPINFO's lpReserved2), from which a
// UCRT program (gesher-exec, dalvikvm) gets fds 3 and above at the same
// numbers. Handles are duplicated inheritable for the call only; nothing else
// is inherited. A missing 0-2 is NUL. Returns 0 or the Win32 error.
DWORD SpawnWithFds(const Launch& launch, const wchar_t* environment, const std::wstring& cwd,
                   const std::vector<ChildFd>& fds, PROCESS_INFORMATION* info, DWORD extra_flags = 0);

// CreateProcessW for `launch` with exactly the three standard handles
// inherited (they must be inheritable; duplicates and null are fine), no
// console window, `environment` a UTF-16 block (or null: this process's) and
// `cwd` (or empty: this process's). Returns 0 or the Win32 error.
DWORD Spawn(const Launch& launch, const wchar_t* environment, const std::wstring& cwd,
            HANDLE std_in, HANDLE std_out, HANDLE std_err, PROCESS_INFORMATION* info);

// A UTF-16 environment block from "NAME=value" strings, sorted by name as
// Windows expects.
std::wstring EnvironmentBlock(const std::vector<std::string>& entries);

// An exit status as Linux code sees it: the low 8 bits for exit codes
// gesher-exec and most programs use, 128 + signal for a crash (NTSTATUS
// exception codes: access violation -> SIGSEGV, ...), the code itself
// otherwise.
int ExitStatus(DWORD code);

// A Windows exit code as waitpid's status word: a death by signal (128 + N,
// N 1..31, the posix layer's and the launcher's convention, or an exception
// code) is WIFSIGNALED with WTERMSIG N; anything else WIFEXITED with the low
// 8 bits. (A program that calls exit(128 + N) itself reads as signalled.)
int WaitStatus(DWORD code);

}  // namespace gesher::exec
