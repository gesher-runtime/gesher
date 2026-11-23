# 0086 — A stand-in libart.so for packers that hook ART by symbol

**Status:** proposed, 2026-11-16 (native-libraries track)

Follows [0084](0084-native-libs-self-unpacking-code.md), which gets
Ijiami's packed shell libraries to unpack and run.
Code: `compat/bionic/art_shim.cc`, `compat/bionic/jni_bridge.cc`,
`patches/art/0800-packer-load-class-hook.patch`,
`patches/art/0801-packer-jni-caller-abi.patch`, `compat/posix/src/procfs.cc`,
`compat/framework/android_runtime.cc`, `compat/arm64/dll/patch-dynarmic-fpsr.py`;
for the arm64 stand-ins (see the last section)
`patches/art/0802-packer-runtime-verifier.patch` and
`patches/art/0803-packer-open-common-hook.patch`.

## Context

With 0084, Soundcore 3.9.7 (`com.oceanwing.soundcore`, packed by Ijiami/爱加密)
unpacks `libexec_x86.so` and `libexecmain_x86.so` and loads its application dex
from memory (`Anonymous-DexFile@…`). ART then rejects the first class it
verifies:

    VerifyError: Verifier rejected class com.oceanwing.common.application.BaseApplication:
    void BaseApplication.<init>() failed to verify: [0x0] Constructor returning
    without calling superclass constructor

The dex file Ijiami hands ART has its method bodies left out. A memory
snapshot of the running shell (its code after UPX unpacked it; its strings are
XOR-encrypted and decrypted on first use) shows how the bodies come back on a
device:

- The shell's hook installer reads `ro.build.version.sdk`. From SDK 31 on it
  first hooks `art::ClassLinker::LoadClass(Thread*, const DexFile&, const
  dex::ClassDef&, Handle<mirror::Class>)`
  (`_ZN3art11ClassLinker9LoadClassEPNS_6ThreadERKNS_7DexFileERKNS_3dex8ClassDefENS_6HandleINS_6mirror5ClassEEE`).
  Only if that fails does it try `ClassLinker::LoadMethod` in its ObjPtr
  and MethodAnnotationsIterator forms, whose replacement calls
  `NterpGetCodeItem` on the loaded `ArtMethod`.
- The hook function resolves a symbol by itself: it reads `/proc/self/maps`
  with raw `openat`/`read`, takes the line whose path ends in `/libart.so`
  (format `%lx-%lx %*s %*s %*s %*s %s`: permissions are not looked at), and
  reads the symbol from that object. If it finds nothing it returns -1, and
  the installer goes on silently.
- The `LoadClass` replacement first calls a function with `(dex_file,
  class_def)` and then jumps to the original. That function reads
  `DexFile::begin_` (offset 8: after the vtable pointer, as in Android 16's
  `art::DexFile`) and `ClassDef::class_data_off_` (offset 0x18), walks the
  class data and writes each method's code back at `begin_ + code_off`. It
  needs nothing else from ART.

Gesher's ART is linked into `dalvikvm.exe`; there is no ELF `libart.so` and so
no line in `/proc/self/maps`. Every hook attempt fails at the resolver (its
later strings were still encrypted in the snapshot: the code after it never
ran), and ART reads the empty bodies.

On a device running Android 16 only the `LoadClass` hook can succeed:
`LoadMethod` became `ClassLinker::LoadClassHelper::LoadMethod`, inline, with
no symbol.

## Decision

**A stand-in `libart.so` for such an app, and ART calling it.**

- `compat/bionic/art_shim.cc` writes a small ELF64 x86-64 object to
  `$ANDROID_DATA/system/gesher-art/<hash of its bytes>/libart.so` (once; a
  process that has an older one mapped keeps it) and loads it with the ELF
  loader. It exports only the `LoadClass` symbol, a System V function that
  does nothing: a normal prologue, two 8-byte NOPs and the epilogue. An inline
  hook can displace the first 14 bytes (or 5) on whole, position-independent
  instructions.
- What the shell's parser needs is there:
  - one read-execute `PT_LOAD` from file offset 0, so the maps line's start
    holds the ELF header;
  - `PT_DYNAMIC` with `DT_SYMTAB`, `DT_STRTAB`, `DT_HASH` and `DT_GNU_HASH`;
  - section headers with `.dynsym` and `.symtab`.

  The file is padded to whole pages. gesher-base maps whole pages of a file
  as a view, which `/proc/self/maps` names, and copies a final partial page
  into anonymous memory, which it cannot name. That was the first failure
  found when this was tried under gesher-exec.
- `compat/bionic/jni_bridge.cc` loads the stand-in when an app's
  `System.load`/`loadLibrary` opens a packer's shell (base name `libexec.so`,
  `libexecmain.so`, `libexec_x86.so` or `libexecmain_x86.so`). It loads it
  before the shell's own code runs, after checking that the input is an
  x86-64 ELF shared object; ARM64 shells with those generic names do not
  activate this host x86-64 stand-in. It sets ART's `gesher_art_load_class_hook`
  to the stand-in's `LoadClass`. Other apps get neither the file nor the hook.
  `GESHER_ART_SHIM=0` turns it off.
- `patches/art/0800` defines that pointer in `runtime/class_linker.cc`. When
  it is set, `ClassLinker::LoadClass` calls it first for a class not loaded by
  the boot class loader. It passes the device's arguments in the System V ABI:
  `this`, `self`, `&dex_file`, `&dex_class_def`, and the `Handle` by value.
  The packer's replacement runs there and calls the original, which returns
  at once, and ART goes on to read the class data.

**What Soundcore needed after that** (found running it on a local build, in
order):

- **`/proc/self/cmdline` names the app's process.** `SoundCoreApplication.onCreate`
  returns at once unless blankj's `ProcessUtils.isMainProcess()` (the process
  name from `/proc/self/cmdline` equals the package name) holds, and the app
  then fails in its first activity on views its `Application` never set up.
  `Process.setArgV0Native` was empty and `/proc/self/cmdline` was dalvikvm.exe's
  Windows command line. Now `setArgV0` (from `handleBindApplication`, and from
  `GesherAppDriver` in single-process mode) records the name
  (`gesher/process_name.h`). `/proc/self/cmdline` is then `name\0` and
  `/proc/self/comm` is the name shortened as Android's `SetThreadName` does,
  as on a device. The calling thread is named the same way.
- **ART's own JNIEnv in loaded code.** Ijiami makes Java methods native by
  writing their `ArtMethod` (`kAccNative` and the JNI entry; "registerHijack")
  rather than through `RegisterNatives`. ART then calls its functions with
  ART's JNIEnv instead of the proxy the bridge's entry stub would give. The
  shell calls that table with the System V ABI: a SIGSEGV in
  `art::JNI::ExceptionCheck`, whose Microsoft ABI reads the JNIEnv from `rcx`.
  When the stand-in is loaded, the bridge installs a JNIEnv table
  (`JNIEnvExt::SetTableOverride`, `patches/art/0801`) whose every entry is a
  stub that chooses by its return address:
  - inside dalvikvm.exe (Gesher's code, including the proxy's functions), ART's
    Microsoft ABI function;
  - anywhere else (loaded ELF code), the bridge's System V function.

  The stub uses only r10 and r11, so `%al` of a System V variadic call
  survives. The bridge's `Real()` now passes ART's own JNIEnv through
  unchanged. As `SetTableOverride` does anywhere, this turns ART's core
  platform API enforcement off in that process.
  Installation checks both allocations, memory protection and instruction
  cache flushing before publishing the table. A failed attempt releases its
  allocations and can be retried. The class hook is published with release/
  acquire synchronization only after the complete JNI table is ready.
- **`MRS CNTVCT_EL0` in arm64 code.** SQLCipher (arm64-v8a, translated) reads
  the virtual counter, which the pinned dynarmic left undefined (SIGILL).
  `compat/arm64/dll/patch-dynarmic-fpsr.py` decodes it as `CNTPCT_EL0`,
  equal to it with Linux's zero `CNTVOFF_EL2`, and `arm64_smoke_test` checks
  it.

- **`fork()` in arm64 code fails with `EAGAIN`.** Ijiami's runtime check
  (`libijmdetect-drisk.so`, arm64) forks twice: a child that installs a
  SIGABRT handler (one that SIGKILLs itself) and sends itself SIGABRT with a
  raw `kill`, and a child that compares `sigaction` with raw `rt_sigaction`
  and leaves with a raw `exit`; the parent `wait`s for each. The shim's
  vfork-style child (0055) ran that `kill` in the app's own process, which
  died of SIGABRT. In guest code that fork was never sound anyway: its second
  return goes back to the host frame that called it, with the guest's
  registers as the child left them. `compat/arm64/dll/guest_libc.cc` now
  fails it (`EAGAIN`, as Linux does at `RLIMIT_NPROC`); both checks then see no
  child and pass. Found with a new report: a SIGABRT (raised or from
  `abort()`) on a guest thread prints its registers and backtrace
  (`gesher_arm64_print_guest_state`, called from `signal_bionic.cc`).
- **The copied-`syscall` lookup reads its window once.**
  `CopiedSyscallLengthLocked` (0084) read memory with one `ReadProcessMemory`
  per fingerprint and then per byte. The rewrite of a page on execution calls
  it for every `ud2` it decodes, holding `g_lazy_lock`; with Ijiami's many
  sites one page took minutes. A thread ART counted as runnable (the shell's
  `LoadClass` replacement, calling `mprotect`) waited for that lock, and ART's
  4-second suspend timeout aborted the process. It now reads the 47 bytes
  around the instruction a page at a time and compares every fingerprint in
  memory: Soundcore's launch went from 130-150 s to 12 s.

With all of these, Soundcore 3.9.7 starts and shows its first screen, its
terms of use, and then its main and sign-in screens, on a local build. Its own
signature check (`libscsecurity.so`, `SCSecurity_CPP`) reads
`PackageManager.getPackageInfo(...).signatures` and reports the re-signed APK
used here (v2 only, AOSP test key) as not Anker's. With the check on, as the
app sets it, `SCSecurityLib.getAppRequestSignSecret` then returns an empty
string, so the server rejects every signed request (sign-up fails with "an
error occurred"); with the check off it returns a 32-character key. That APK
carries no Anker certificate, so a phone gives the same result; with Anker's
APK the check passes.

## Consequences

- An Ijiami-packed app can get its method bodies as on an Android 16 device.
  Whether a given app then runs depends on the rest of its shell. The same
  snapshot shows anti-tamper scans of `libart.so`'s code, and checks for
  Frida, Xposed, FART-style unpackers and emulators. Other packers that hook
  ART by symbol (Jiagu, Legu, Bangcle) need their shell's names added and
  possibly other symbols.
- **Cost for other apps:** one null-pointer test per `LoadClass`, and nothing
  on disk.
- **For a packed app:** a 4 KiB file once, and one indirect call per class
  loaded by an app class loader.
- `/proc/self/maps` shows the stand-in as `r-xs` (gesher-base reports a view
  that is not copy-on-write as shared); a device shows `r-xp`. The shell's
  resolver skips the permissions; its anti-tamper scan only needs `r`.
- **Tests:** `tests/native/art_shim_test.cc` and `art_hook_test.so`
  (tracks/40-native-libs.ps1) do what the shell does:
  - find the maps line;
  - resolve the symbol through `DT_GNU_HASH`, `DT_HASH` and the file's
    `.symtab`;
  - hook it inline over its first whole instructions;
  - check that ART's call reaches the hook with `begin_` and `class_data_off_`
    where the shell reads them, and returns through the trampoline.

  ART's own call is exercised only by an app whose packer hooks it; no packed
  app is in the repository.
  `tools/test_art_jni_dispatch.py` executes the generated dispatch code with
  both ABIs and checks integer, floating-point, stack and variadic arguments;
  controlled OS/VM boundaries cover failure cleanup, retries and concurrent
  publication, including rejection of unsupported or incomplete ELF headers.
  The ARM64 smoke test calls both guest `fork` and `vfork` bindings
  and requires `-1` with `EAGAIN`.
- **Not covered:** the `LoadMethod` fallbacks (no such function exists in
  Android 16's ART), and any packer that resolves ART symbols through
  `dl_iterate_phdr` or `dlopen("libart.so")` instead of `/proc/self/maps`.

## Addendum (2026-11-23): an arm64 stand-in for NetEase Yidun

Dreame (`com.dreame.smartlife`, issue #301) is packed by NetEase Yidun
(网易易盾). Its shell, `libnesec.so`, exists only for arm64-v8a and runs
translated (ADR 0047). It resolves ART's symbols the way Ijiami's shell does:
from the objects `/proc/self/maps` names. But it looks each symbol up in the
library that defines it on a device:

- in `libart.so`: `ClassLinker::LoadClass`, which it hooks inline;
  `Runtime::DisableVerifier`, which it calls on `Runtime::instance_` before
  it opens its decrypted dex files from memory (ART then opens them without
  verifying them, as `OatFileManager::OpenDexFilesFromOat` does when
  verification is disabled); `Runtime::IsVerificationEnabled` and
  `IsVerificationSoftFail`; and `OatHeader::kOatVersion`;
- in `libdexfile.so`: `DexFileLoader::OpenCommon` (Android 16's form, with a
  `DexFileContainer`), which it hooks inline to learn where ART placed each
  dex file it opens. Its `LoadClass` hook finds a dex file's method-body
  store by that address. A stand-in `libart.so` that exported `OpenCommon`
  was not looked at.

**Decision.** For an arm64 ELF shell named `libnesec.so`, the bridge writes
and loads two arm64 stand-ins in one directory named after their contents,
`libart.so` and `libdexfile.so`, as a device's ART module has them in its
`lib64` (`compat/bionic/art_shim.cc`, `LoadArtShimArm64`;
`compat/bionic/jni_bridge.cc`, `LoadArtShimArm64For`). The x86-64 stand-in is
unchanged, and an arm64 shell with Ijiami's generic names still gets nothing.

- **`libart.so`** exports `LoadClass` (a frame record around NOPs: whatever a
  hook displaces is position-independent); the three Runtime functions, each
  six displaceable NOPs and then a tail call through its own GOT slot
  (`LDR X16`, `BR X16`; `R_AARCH64_GLOB_DAT`); `kOatVersion`, which holds
  ART's own version; and `Runtime::instance_`, which holds the Runtime
  pointer. ART calls `LoadClass` through a host entry into the translator
  (`gesher::arm64::HostEntry`), as `patches/art/0800` calls the x86-64 one.
- **The Runtime functions act on the real Runtime.** Their GOT slots are
  bound to host functions that call `patches/art/0802`'s
  `gesher_art_runtime_disable_verifier` and
  `gesher_art_runtime_verification`. These act only on this process's
  `Runtime::Current()`; for any other pointer they log it and do nothing
  (or report false). The verifier is disabled only because the app asked
  ART to do so, through ART's own function.
- **`libdexfile.so`** exports `OpenCommon` (six displaceable NOPs, then a
  call through its GOT slot to ART's real `OpenCommon`, with AAPCS64's
  result storage in X8 passed first) and an entry. `patches/art/0803` makes
  ART's `DexFileLoader::OpenCommon` call that entry, through a host entry,
  when the hook pointer is set. The entry makes the device's AAPCS64 call to
  the stand-in's `OpenCommon`, so the shell's inline hook runs as on a
  device. Whatever it does, the dex file is opened by ART's own `OpenCommon`
  (`gesher_art_dex_file_loader_open_common`; a thread-local guard keeps that
  call from going round again), with the arguments the hook passed.
- **Why two objects.** The shell finds `OpenCommon` only in the maps line
  whose path ends in `/libdexfile.so`. Each stand-in therefore exports exactly
  what its library exports on a device, and nothing else.

**What else Dreame needed** on the way to its first screen (separate commits,
each a general Linux or Android contract, not specific to this app):
`execl`/`execlp`/`execle`; the translator honouring the guest's `IC IVAU`,
`IC IALLU` and an executable-range `mprotect` (the shell's inline hooks
rewrite code that already ran); `fsync` on a read-only descriptor; `IP_PKTINFO`
on IPv4 UDP; a content provider authority given to the first provider in a
package that declares it (AOSP's `ComponentResolver`); and bionic's
`libstdc++.so` exports, which the shell `dlopen`s.

**Consequences.**

- With the stand-ins Dreame's shell restores its method bodies and the app
  reaches its welcome (login) screen on a local build. Its later screens and
  its anti-tamper checks were not explored.
- Cost: two files of a few KiB once for such an app, and an atomic load per
  `OpenCommon` for every app (null unless the stand-in was loaded).
- **Tests:** `tests/native/art_shim_test.cc` checks the two arm64 images
  without a translator: ELF header and machine, segments, sonames, the
  exports through `DT_GNU_HASH` and `DT_HASH` (and that neither object
  exports the other's), the `R_AARCH64_GLOB_DAT` GOT relocations and the code
  that reads those slots, and the entry's call. The translated execution of
  the stand-ins and the ART patches are exercised only by an app with this
  shell; no such app is in the repository.
- **Not covered:** an x86-64 or 32-bit NetEase shell, and any ART symbol the
  shell looks up on a path Dreame's startup did not take.
