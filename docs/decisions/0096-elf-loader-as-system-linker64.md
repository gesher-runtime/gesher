# 0096 — Gesher's ELF loader as /system/bin/linker64

**Status:** decided by the maintainer, 2026-11-25 (native-libraries track,
issue #480).

Related: [0047](0047-arm64-translation.md) (ABIs Gesher reports),
[0084](0084-native-libs-self-unpacking-code.md) and
[0086](0086-packer-art-hooks.md) (Ijiami's shell),
[0095](0095-jiagu-shell-abi-selection.md) (another packer's ABI choice).
Code: `compat/posix/src/system_linker.cc`, `compat/posix/src/path.cc`
(`MapPath`), `compat/posix/src/procfs.cc` (`Maps`, `MappedFile`,
`FdTarget`), `compat/elf/exec/exec_spawn.cc` (`PrepareProgram`).

## Context

Anker Soundcore 5.0.39 (`com.oceanwing.soundcore`, packed by Ijiami) exits at
launch (issue #480):

    UnsatisfiedLinkError: No implementation found for java.lang.ClassLoader
      s.h.e.l.l.N.al(java.lang.ClassLoader, android.content.pm.ApplicationInfo, ...)
      at s.h.e.l.l.A.instantiateApplication

Its Java shell (`s.h.e.l.l.S`, disassembled from `classes.dex`) picks which
`assets/ijm_lib/<abi>/libexec.so` and `libexecmain.so` to extract to `files/`
and `System.load`:

1. `ld()` opens `/system/bin/linker` if it exists, else
   `/system/bin/linker64`, reads 20 bytes and switches on byte 18
   (`e_machine`): 0xB7 arm64-v8a, 0x03 x86, 0x28 armeabi, 0x3E x86_64; on any
   exception it returns "".
2. If that is "", `a()`: `Arrays.toString(Build.SUPPORTED_ABIS)` contains
   `x86` ? `x86` : `ro.product.cpu.abi`.
3. Then, if the result contains `x86`: `il()` ? `x86_64` : `x86`; otherwise
   `il()` ? `arm64-v8a` : `armeabi`.
4. `il()` reads `/proc/self/maps` line by line: true if a line contains
   `/lib64/libart.so`, `/lib64/libaoc.so` or `/bin/linker64`.

On a 64-bit device the linker is `/system/bin/linker64` (a link to the
runtime APEX's), and every process maps it, so the shell takes the 64-bit
library of the device's machine. On Gesher neither file existed and
`/proc/self/maps` had no such line: ART and the ELF loader are inside
`dalvikvm.exe`. `SUPPORTED_ABIS` is `x86_64,arm64-v8a`, so the shell took
`x86` and then, with `il()` false, the 32-bit i386 `libexec.so`, which Gesher
cannot load. Its natives were never registered. The x86-64 `libexec.so` is in
the APK; an older Soundcore (3.9.7, 0086) used an x86-64 shell and ran.

`il()` is the shell's test for a 64-bit process. Its answer was wrong for
Gesher: the process is 64-bit and has a dynamic linker, Gesher's ELF loader
(`compat/elf/elf_loader.cc`), which loads x86-64 ELF natively. It just had no
file name.

## Decision

**Gesher's ELF loader appears as `/system/bin/linker64`**, a desktop
adaptation decided by the maintainer and listed in CONTRIBUTING.md's
examples. It is generic: every process, no package or packer is singled out.

- **The file.** `/system/bin/linker64` is a read-only file holding a small
  ELF image (`system_linker::Image()`): ELF64, little-endian, `ET_DYN`,
  `EM_X86_64`, the machine Gesher's linker loads natively. It names itself as
  Gesher's: an `NT` note with owner `Gesher` ("Gesher ELF loader: the dynamic
  linker of this process, part of Gesher's runtime...") and a `.comment`
  "Gesher ELF loader". It has one read-only `PT_LOAD`, no code and no entry
  point. `MapPath` sends the name to
  `$ANDROID_DATA/system/gesher-linker/<hash of the bytes>/linker64` (else
  `%LOCALAPPDATA%\Gesher\data`), written on first use, as the stand-in
  `libart.so` is (0086): a process that has one mapped keeps it, and a file
  holding other bytes is replaced. Writing through the name fails
  (`EACCES`).
- **No `/system/bin/linker`.** Gesher has no 32-bit support; a 64-bit-only
  device has no 32-bit linker either.
- **In `/proc/self/maps`.** `Maps()` first maps the file read-only, once, for
  the rest of the process's life (`EnsureMapped`), and `MappedFile` names
  that view `/system/bin/linker64` (offset 0, the file's inode).
  `/proc/self/fd/<n>` names an open descriptor of it the same way.
  The mapping is made when maps is first read rather than at process start
  because that file is the only place it can be observed (it is not a loaded
  library: `dl_iterate_phdr` and `dlopen` do not see it), and every reader of
  `/proc/self/maps` or `/proc/<own pid>/maps`, Java or native, raw `openat`
  included, gets there through `Maps()`. A process that never reads maps
  pays nothing; one that does pays one comparison of a file of under 1 KiB and
  one `MapViewOfFile`, once.
- **Executing it.** Android runs `linker64 PROGRAM [ARGS...]` as PROGRAM.
  `exec_spawn`'s `PrepareProgram` does that for the path
  `/system/bin/linker64`: PROGRAM (a path, as the linker takes it) must be an
  ELF executable or one of gesher-exec's tools, and gets the same launch as
  when executed directly. No program, an option (`--list`) or a program that
  is not ELF is `ENOEXEC` (Android's linker execs, prints an error and exits).
  The file itself run by gesher-exec (`--elf` on its Windows path) stops with
  `CANNOT LINK EXECUTABLE ... has no entry point`: nothing in it is run.

## Consequences

- Code that asks the linker for the device's machine gets x86-64, and code
  that looks for `/bin/linker64` in its maps sees a 64-bit process. For
  Ijiami's shell: `ld()` returns `x86_64`, `il()` is true, and it extracts the
  x86-64 `libexec.so`, which the existing support (0084, 0086) runs.
  Other apps keep the ABIs they saw (`SUPPORTED_ABIS` is unchanged); code that
  switches on the linker's machine now finds the one it would on an x86-64
  device (0095's packer reads `/system/bin/ls`, not the linker).
- `/proc/self/maps` shows the view as `r--s` (gesher-base reports a view that
  is not copy-on-write as shared); a device shows the linker's segments as
  `r--p`, `r-xp` and `rw-p`. The line is one read-only page range.
- Only the exact name `/system/bin/linker64` is mapped (as `/system/fonts` is):
  `/apex/com.android.runtime/bin/linker64` and `/system/bin` as a directory
  do not exist.
- The file is protected by its read-only attribute, not by the name: an app
  running with the user's token can delete or change it as it can any of the
  user's files (0090). The next process that uses the name writes it again,
  and maps only bytes equal to the image.
- With the AppContainer sandbox (0090, off by default) the container needs
  read access to the file and write access only if it is missing; that is not
  arranged yet, so a contained process may see no linker64 (as before this
  change).
- **Tests:** `tests/native/system_linker_test.cc` (tracks/21-posix.ps1) reads
  the header through `open`, checks `stat`, `access`, the missing
  `/system/bin/linker`, refused writes, the maps line (name, offset, inode,
  the mapped bytes, mapped once), `/proc/self/fd`, the launch `linker64
  PROGRAM ARGS` prepares (equal to PROGRAM's own), the `ENOEXEC` cases, and
  that gesher-exec refuses to run the file. The procfs and SQLite path
  fixtures stub the new functions.
