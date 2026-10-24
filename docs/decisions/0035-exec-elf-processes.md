# 0035 — Android ELF executables as Windows processes

**Status:** accepted, 2026-10-24 (exec track; see docs/exec.md for the CI
runs that verify it). The coordinator may renumber.

## Context
Apps run native programs with `ProcessBuilder`/`Runtime.exec`: an executable
from their `nativeLibraryDir` (packaged as `lib*.so`, since the package
manager only extracts files named so), `sh -c ...`, sometimes system tools.
Simon Tatham's Puzzles generates every game by running
`libpuzzlesgen.so <game> <params>` and reading its standard output.

On Android, libcore's `UNIXProcess` natives (`UNIXProcess_md.c`) `vfork` and
`execve`; the kernel maps the PIE and starts `/system/bin/linker64`, which
loads its `DT_NEEDED` libraries and jumps to `_start`, whose `__libc_init`
calls `main`. Gesher had none of it: `UNIXProcess_md.c` was not built
([0011](0011-libcore-natives.md)), so `UNIXProcess`'s static initializer threw
`UnsatisfiedLinkError`, which Puzzles' executor swallowed — the game view
stayed empty.

What exists: the ELF loader and bionic shim load an NDK `.so` into a Windows
process and run it ([0014](0014-native-libs-fs-base.md),
[0020](0020-native-libs-libandroid.md)); gesher-base fds are CRT fds over
Windows handles; app processes live in the Gesher device's job.

## Options considered
1. **Run the executable inside the app's own process** (load it with the
   loader on a thread, redirect its stdio). No new process, but `exit()` would
   end the app, its globals and signal state would mix with the app's, a
   crash would take the app down, two runs at once would share one copy of
   its data, and `waitFor`/`destroy` would have nothing to wait for or kill.
2. **Port `UNIXProcess_md.c`** over a `fork`/`exec` emulation in gesher-base.
   `fork` cannot be emulated faithfully on Windows (Cygwin's copy-the-address-
   space approach does not fit ART's heap and threads), and nothing needs it.
3. **Implement `UNIXProcess`'s four natives with `CreateProcessW`, and start
   ELF executables in a small launcher process** that loads them with the
   existing loader and shim as its main program.

## Decision
Option 3.

- `compat/libcore/process.cc` registers `forkAndExec`, `waitForProcessExit`,
  `destroyProcess` and `initIDs` on `java.lang.UNIXProcess` (in place of
  `UNIXProcess_md.c`, removed from the build): pipes are `CreatePipe` pairs
  whose parent ends become CRT fds (`_open_osfhandle`), so libcore's streams,
  `available()` and the process reaper are unchanged; the three child handles
  are the only ones inherited (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), so a
  child never holds another child's pipe open; the pid is the Windows process
  id, kept with its handle until reaped; statuses follow `waitpid`'s meaning
  for Java (exit code, 0x80 + signal), `destroy` is 0x80 + SIGTERM.
- `compat/elf/exec/exec_spawn.cc` resolves a program as `execvp` would and
  chooses how it runs by its contents: ELF x86-64 → the launcher, PE → itself,
  `.cmd`/`.bat` → `cmd.exe`, `sh`/`/system/bin/sh`/`#!...sh`/plain text → the
  launcher's shell.
- `gesher-exec.exe --elf` is the dynamic linker's stand-in: it loads the PIE
  through `gesher::elf::Load` (ET_DYN like a library; the loader now records
  `e_entry`, `PT_INTERP` and `DT_PREINIT_ARRAY`, and init functions get
  `(argc, argv, envp)` once `SetInitArguments` is called), lays out Linux's
  initial stack (argc, argv, envp, auxv) on a thread with a committed 8 MiB
  stack, and jumps to `_start` with a five-instruction trampoline. The NDK's
  `crtbegin` calls `__libc_init`, which the launcher provides (with `exit`,
  `_exit`, `getauxval`, `environ`, `__progname`) by adding to the shim's
  symbol table before the load, so the shim itself is unchanged.
- A minimal shell in the launcher covers `sh -c` with simple commands and
  `;`/`&&`/`||` — what apps pass to `sh` in practice — and refuses the rest
  with status 2 rather than guessing.

## Consequences
- Each ELF run costs a process start and a load of the executable and its
  libraries (Puzzles' `libpuzzlesgen.so` needs `libpuzzles.so`, ~1 MB); the
  loader copies segments rather than mapping them, which is fine at this size.
- Children inherit the app process's job, so `gesher-app shutdown` and the
  system server ending an app take its children with them.
- An executable's own ELF thread-locals (local-exec `%fs:-N`) are not
  supported (see docs/exec.md); emulated TLS and TLS in its libraries are.
- `fork`/`exec`/`system`/`popen` from native code remain unimplemented; they
  can reuse `exec_spawn` later.
