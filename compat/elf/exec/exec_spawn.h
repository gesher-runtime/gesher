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
//     without "#!", as Linux's execvp does) run in gesher-exec's minimal
//     shell; other "#!" interpreters are looked up like programs.
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

}  // namespace gesher::exec
