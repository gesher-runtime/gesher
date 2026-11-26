# Phase 6: native x86-64 libraries. Dot-sourced by run-tests.ps1, so Check,
# NoDrive, $Dir, $root, $bcp and $failed are in scope.

# Can Windows user code set the %fs base (WRFSBASE), for bionic's TLS and the
# stack-protector cookie? This is the open question of ADR 0014. It is reported
# as INFO, not a pass/fail gate: a .so that does not touch %fs (like the step-1
# trivial one) loads and runs regardless, and the next session resolves %fs
# before a libc++ / stack-protected .so needs it. The probe is crash-safe and
# prints its own verdict.
$fsOut = & "$Dir/fsbase_probe.exe" 2>&1 | Out-String
if ($fsOut -match 'all passed') {
  Write-Host 'INFO  fsbase: %fs IS usable via WRFSBASE on this runner'
} else {
  Write-Host 'INFO  fsbase: %fs NOT usable via WRFSBASE here (ADR 0014: the loader rewrites %fs instead)'
}
($fsOut -split "`n" | Where-Object { $_ -match 'ok|FAIL|verdict|#UD' } | Select-Object -First 12) |
  ForEach-Object { Write-Host "      $_" }

# The ELF loader maps a self-contained x86-64 Android .so (built by the NDK in
# native-libs.yml), applies its relocations, runs its constructors and calls
# its exports. This is the step-1 gate.
Check 'ELF loader: load a trivial .so and call it' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/trivial.so")
} @('elf_load_test all passed')

# dlopen()'s dlerror() (what System.load's UnsatisfiedLinkError carries): a
# missing or non-library path in Android's linker words, Windows DLLs in Windows'.
Check 'dlopen: Android linker errors for app paths, Windows errors for Windows DLLs' {
  & "$Dir/dlopen_errors_test.exe"
} @('dlopen-errors: PASS a missing path is "library ... not found"',
  'dlopen-errors: PASS another file has bad ELF magic',
  'dlopen-errors: PASS a PE image that fails keeps the Windows error',
  ' assertions, 0 failures')

# Public loading/lookup/iteration on real ELF dependency graphs. The second
# executable changes only the test copy's protection primitive for one failure.
foreach ($transactionExe in 'elf_load_transaction_test.exe', 'elf_load_transaction_failure_test.exe') {
  Check "ELF loader: graph rollback, retry, cycles, constructors and concurrent publication ($transactionExe)" {
    & "$Dir/$transactionExe" (NoDrive "$Dir/elf-load-transaction")
  } @('elf_load_transaction_test all passed')
}

# textrel.so contains dynamic relocations into a read-only pointer table.
# Its constructor and selftest both require the relocated pointers to work.
Check 'ELF loader: TEXTREL pages relocate before constructors and selftest' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/textrel.so")
} @('gesher_selftest() reports no failures', 'elf_load_test all passed')

# A thread that runs loaded code has its stack committed (compat/elf/tls.cc,
# CommitStack): code built for Linux allocates large frames without probing
# (libonnxruntime.so's `sub $0x119c0,%rsp`), and with %rsp past the guard page
# Windows cannot deliver the fault and ends the process without a word.
# elf_load_test makes such a frame (256 KiB) on a fresh thread after its TLS setup.
Check 'ELF loader: a loaded-code thread takes a large frame without stack probes' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/trivial.so")
} @('unprobed 256 KiB frame on a loaded-code thread ok', 'elf_load_test all passed')

# The bionic shim: a .so built against bionic's libc, libm, libdl, liblog and
# pthread (tests/native/libc_test.c) loads through the ELF loader and passes
# its own checks: memory, strings, printf/scanf through the System V vararg
# bridge, errno, time, qsort, threads, mutexes, condition variables, keys,
# dlsym and dl_iterate_phdr. With Android's CA certificates next to
# elf_load_test.exe (cacerts\, tools/net/collect.sh), it also requires
# /system/etc/security/cacerts and /apex/com.android.conscrypt/cacerts to be
# that directory, as Dart's dart:io and BoringSSL read them
# (compat/bionic/system_cacerts.h).
$env:GESHER_TEST_SYSTEM_CACERTS = if (Test-Path "$Dir/cacerts/*.0") { '1' } else { '' }
Check 'bionic shim: a libc/pthread .so runs its checks' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/libc_test.so")
} @('libc_test: all passed', 'sleep contracts: complete', 'elf_load_test all passed',
    'Bionic realtime range contracts: all passed', 'Bionic tgkill contracts: all passed')
Remove-Item Env:GESHER_TEST_SYSTEM_CACERTS -ErrorAction SilentlyContinue

# Key and thread_local destructors of the bionic shim on threads it did not
# start (ART's, and Windows threads host code calls back on): they run in the
# rounds of ART's winpthreads key, so a library's destructor that detaches
# from the VM runs before ART's key comes round again, which aborted Waze
# ("Native thread exited without calling DetachCurrentThread").
Check 'bionic shim: key destructors detach a thread before ART checks it, on every kind of thread' {
  & "$Dir/bionic_key_exit_test.exe"
} @("ok    bionic keys: a shim-started thread detaches before ART's key comes round again",
    "ok    bionic keys: a winpthreads thread detaches before ART's key comes round again",
    "ok    bionic keys: a Windows thread detaches before ART's key comes round again",
    'Bionic concurrent key read contracts: all passed',
    'Bionic concurrent key write contracts: all passed',
    'bionic_key_exit_test all passed')

# %fs without WRFSBASE (ADR 0014): NDK libraries built with the default stack
# protector and thread-locals (tests/native/fs_test.c). The loader rewrites
# their %fs accesses to %gs-based ones at load time; the libraries check the
# guard, the thread pointer and their thread-locals across sleeps and threads.
# API 26: emulated thread-locals. API 29: ELF TLS (general-dynamic,
# local-dynamic, initial-exec), and a TLS library dlopen()ed while a thread
# runs.
Check 'native libs: stack-protected .so with emulated TLS (API 26) under the %fs rewrite' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/fs_test26.so") (NoDrive "$Dir")
} @('fs_test: all passed', 'elf_load_test all passed')

Check 'native libs: stack-protected .so with ELF TLS (API 29), threads and a late TLS library' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/fs_test29.so") (NoDrive "$Dir")
} @('fs_test: all passed', 'elf_load_test all passed')

# android_dlopen_ext (tests/native/dlext_test.c): extended flags checked as
# bionic checks them; RESERVED_ADDRESS loads at the start of the caller's
# reservation or fails without touching it; RESERVED_ADDRESS_HINT falls back
# to any address; a load failing after mapping leaves the reservation
# PROT_NONE and the caller's; unimplemented flags fail with dlerror.
# Run again without placeholder mappings, where the loader copies segments
# instead of mapping file views (and maps the gaps between them PROT_NONE).
Check 'native libs: android_dlopen_ext reserved address space, hints, dlclose and flag checks' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/dlext/dlext_test.so") (NoDrive "$Dir/dlext")
} @('dlext_test: all passed', 'elf_load_test all passed')
$env:GESHER_MMAP_NO_PLACEHOLDERS = '1'
Check 'native libs: android_dlopen_ext with copied segments (no placeholder mappings)' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/dlext/dlext_test.so") (NoDrive "$Dir/dlext")
} @('dlext_test: all passed', 'elf_load_test all passed')
Remove-Item Env:GESHER_MMAP_NO_PLACEHOLDERS

Check 'native libs: partial unwind covers hidden code and preserves executable data' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/fs_partial_unwind.so")
} @('partial_unwind: hidden uncovered function reached by direct call PASS',
    'partial_unwind: hidden uncovered function reached by indirect pointer PASS',
    'partial_unwind: runtime recovery leaves original code bytes intact PASS',
    'partial_unwind: FS-like executable data is unchanged PASS',
    'partial_unwind: all passed', 'elf_load_test all passed')

# Raw Linux system calls and code made at run time (ADR 0084). Packers'
# stubs and anti-tamper code use `syscall` instructions, which would enter the
# Windows kernel with Linux numbers; the loader turns each into a trap the
# bionic shim emulates. Code a library writes into its own pages and makes
# executable, and the code of a library that unpacks itself (UPX, as Ijiami's
# libexec_x86.so is packed), is rewritten when it first runs.
Check 'ELF loader: raw syscalls emulated; code made executable at run time rewritten when it runs (hand-assembled)' {
  & "$Dir/elf_deferred_code_test.exe"
} @('unpacking: a syscall stub reached only by `ret` is emulated',
    'elf_deferred_code_test all passed')

Check 'native libs: raw syscalls, syscall(2), ptrace and regex from a .so; code it patches into itself is rewritten' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/raw_syscall_test.so")
} @('raw_syscall: raw syscall in code written at run time PASS',
    'raw_syscall: stack guard in code written at run time PASS',
    'raw_syscall: all passed', 'elf_load_test all passed')

# AF_UNIX socketpair credentials (ADR 0088): SO_PEERCRED, SO_PASSCRED and
# SCM_CREDENTIALS on a SOCK_SEQPACKET pair, whose framed datagrams keep their
# boundaries, MSG_TRUNC, MSG_PEEK and FIONREAD; SCM_RIGHTS still refused.
Check 'native libs: AF_UNIX socketpair credentials (SO_PEERCRED, SO_PASSCRED, SCM_CREDENTIALS) from a .so' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/unix_credentials_test.so")
} @('unix_credentials: all passed', 'elf_load_test all passed')

# SCM_RIGHTS (ADR 0088 phase 2): pipes, memfds and sockets sent on SOCK_SEQPACKET
# and SOCK_STREAM pairs through a binderd of the test's own, and without one. On
# a SOCK_STREAM pair descriptors come with the read that returns the first byte
# sent with them, never before it, also after the receiving end has moved.
Check 'native libs: SCM_RIGHTS through the descriptor broker from a .so' {
  $device = 'rights-' + [guid]::NewGuid().ToString('N')
  $broker = Start-Process -FilePath "$Dir/binderd.exe" -ArgumentList $device -PassThru -WindowStyle Hidden
  try {
    Start-Sleep -Milliseconds 500
    $env:GESHER_BINDER = $device
    & "$Dir/elf_load_test.exe" (NoDrive "$Dir/unix_rights_test.so")
  } finally {
    Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue
    Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
  }
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/unix_rights_test.so")
} @('a read of the ten bytes gets no descriptor', 'the moved end reads the byte before it without a descriptor',
    'unix_rights: all passed', 'unix_rights: all passed (without a broker)', 'elf_load_test all passed')

# What getenv returned and what environ pointed at keep their bytes after the
# variable is putenv()ed, setenv()ed or unsetenv()ed again, as bionic's
# (mozglue's own getenv reads environ, and it caches one).
Check 'native libs: getenv results and environ entries outlive later setenv/putenv/unsetenv from a .so' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/environment_lifetime_test.so")
} @('the first result keeps its bytes after putenv', 'the first environ entry keeps its bytes after a new value',
    'environment_lifetime: all passed', 'elf_load_test all passed')

# bionic's libstdc++.so (tests/native/libstdcxx_test.cc): dlopen("libstdc++.so")
# and dlsym of exactly its exports, operator new/delete crossing malloc/free,
# the static-local guard protocol with threads racing on one guard (abort and
# retry included) and compiler-generated guards bound through DT_NEEDED.
Check 'native libs: bionic libstdc++.so (new/delete, __cxa_guard_*, __cxa_pure_virtual) from a .so' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/libstdcxx_test.so")
} @('ok   libstdcxx: waiters announced themselves','libstdcxx: all passed', 'elf_load_test all passed')

# An app library with its own libc++_shared.so keeps libc++'s operator new and
# guards for its relocations and dlsym(RTLD_DEFAULT); libstdc++.so stays the platform's.
Check "native libs: an app's libc++_shared.so keeps its definitions next to libstdc++.so" {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/libstdcxx-order/libstdcxx_order_test.so") (NoDrive "$Dir/libstdcxx-order")
} @('libstdcxx_order: all passed', 'elf_load_test all passed')

# The NDK's MediaDrm/MediaCrypto with no DRM plugin: no scheme is supported and
# no object is created, as Android answers for an unsupported scheme.
Check 'native libs: MediaDrm/MediaCrypto report no crypto scheme from a .so' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/media_drm_test.so")
} @('AMediaDrm_isCryptoSchemeSupported is false for every scheme', 'media_drm: all passed',
    'elf_load_test all passed')

Check 'native libs: a UPX-packed .so unpacks itself with raw syscalls and runs its checks' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/raw_syscall_upx.so")
} @('unpacks its code at run time (UPX)', 'code unpacked at run time: yes',
    'raw_syscall: all passed', 'elf_load_test all passed')

# A memfd whose size ends inside a page: what a shared mapping writes past EOF
# in that last page is seen by the next mapping (as on Linux, where UPX 5
# writes its return stub there), without growing the file; ftruncate to the
# same size discards it.
Check 'native libs: memfd final-page bytes survive remapping without extending the file' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/memfd_tail_test.so")
} @('memfd_tail: all passed', 'elf_load_test all passed')

# ADR 0086: a packer's shell (Ijiami) finds libart.so in /proc/self/maps and
# hooks ClassLinker::LoadClass by its symbol. art_hook_test.so does what the
# shell does to Gesher's stand-in libart.so, then ART's call reaches the hook.
# The arm64 stand-ins for NetEase Yidun (libart.so, libdexfile.so) are checked
# as images first: exports, sonames and GOT relocations.
Check 'native libs: a stand-in libart.so a packer finds in /proc/self/maps and hooks by symbol' {
  & "$Dir/art_shim_test.exe" (NoDrive "$Dir/art_hook_test.so") (NoDrive (Join-Path ([IO.Path]::GetTempPath()) 'gesher-art-shim-test'))
} @('PASS  arm64 libart.so: ELF64 LSB AArch64 ET_DYN', 'PASS  arm64 libdexfile.so: DT_SONAME',
    'PASS  arm64 libart.so: each Runtime function tail-calls through its own import''s GOT slot',
    'PASS  arm64 libdexfile.so: one R_AARCH64_GLOB_DAT GOT slot, for ART''s OpenCommon',
    'PASS  arm64 libdexfile.so: none of libart.so''s symbols',
    'PASS  /proc/self/maps names the stand-in at its base',
    'PASS  LoadClass through DT_GNU_HASH', 'PASS  LoadClass through the file''s .symtab',
    'PASS  the hook reads ClassDef::class_data_off_ at 0x18', 'art_shim_test all passed')

Check 'bionic shim: ftruncate rights, cursor and same-size final-page invalidation' {
  & "$Dir/bionic_ftruncate_test.exe"
} @('bionic_ftruncate_test all passed')

Check 'bionic shim: creat and creat64 create or truncate a write-only file' {
  & "$Dir/bionic_creat_test.exe"
} @('bionic_creat_test all passed')

Check 'bionic shim: shared CRT stream ownership and wide stdio' {
  & "$Dir/bionic_stdio_contract_test.exe"
} @('bionic_stdio_contract_test all passed')

# Step 4, the JNI path: dalvikvm runs gesher.JniTest (tests/native/java), whose
# System.load of jni_test.so goes through ART's host libnativeloader path to
# gesher-base's dlopen, the ELF loader and the JNI bridge
# (compat/bionic/jni_bridge.cc): JNI_OnLoad and RegisterNatives, natives found
# by name, arguments on the stack, and the .so's calls back into ART through
# the System V JNIEnv/JavaVM proxies, from ART's threads and its own.
# System.load takes an absolute path.
Check 'native libs: System.load of an NDK .so; JNI_OnLoad, RegisterNatives, Java_* and JNIEnv calls into ART' {
  & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcp" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
    -Xint '-Xusejit:false' -cp (NoDrive "$Dir/jni-test.dex") gesher.JniTest (NoDrive (Resolve-Path "$Dir/jni_test.so").Path)
} @('JNI_OnLoad ran: ok', 'a native thread that ends attached only warns, as on Android: ok',
    'GetEnv guest boundary: all passed', 'GetEnv attach/detach: all passed',
    'JniTest all passed')

# Step 5, libc++: a C++ .so against the NDK's libc++_shared.so, which the
# loader loads from the same directory; C++ exceptions are thrown and caught
# inside the loaded code (libunwind finds .eh_frame through dl_iterate_phdr).
Check 'native libs: libc++_shared through the loader; C++ exceptions thrown and caught in the .so' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/libcxx_test.so") (NoDrive "$Dir")
} @('libcxx_test: all passed', 'elf_load_test all passed')

# Step 5, the test APK: gesher-app runs nativeapp.apk (tests/apps/nativeapp),
# extracts its lib/x86_64 (libnativeapp.so and libc++_shared.so) into the app's
# native library directory, and the Activity's onCreate loads the library with
# System.loadLibrary and calls into it: C++ with an exception caught inside,
# and a call back into the Activity. onResume prints the native result.
$bcpNative = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$nativeAppOut = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpNative" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/nativeapp.apk") 2>&1 | Out-String
$nativeAppExit = $LASTEXITCODE
Check 'native libs: gesher-app runs nativeapp.apk; lib/x86_64 loaded from the APK, native result checked in onResume' {
  $global:LASTEXITCODE = $nativeAppExit
  $nativeAppOut
} @('extracted 2 native libraries (x86_64)',
    'NativeActivity onResume, native=Hello from C++, caught std::out_of_range, callBack(12)=145',
    'gesher-app: com.gesher.nativeapp.NativeActivity onResume',
    'gesher-app: done')

# The same run's platform checks (tests/apps/nativeapp/jni/nativeapp.cpp):
# libandroid (an asset read through AAssetManager_fromJava, AAssetDir,
# AConfiguration, no sensors, ALooper_addFd with a pipe callback and an
# eventfd ident on the library's own thread's looper, ALooper_wake, and a pipe
# on Java's main Looper whose callback the message loop runs) and the libc the
# shim provides (a socket pair with poll and select, TCP over loopback,
# getaddrinfo, readdir with d_type, fscanf/getline, std::filesystem, signals,
# strftime/timegm/mktime, sysconf).
Check 'native libs: gesher-app runs nativeapp.apk; libandroid assets/looper and libc sockets/readdir/std::filesystem/signals from the .so' {
  $global:LASTEXITCODE = $nativeAppExit
  $nativeAppOut
} @('NativeActivity platform: asset=Hello from an asset, assets=hello.txt, config=ok, sensors=0, looper=ok, mainlooper=armed, socketpair=ok, tcp=ok, getaddrinfo=ok, readdir=a.txt/b.txt/sub, fscanf=ok, filesystem=ok, signals=ok, strftime=ok, sysconf=ok, perfhint=ok',
    'NativeActivity main looper: fd callback ran',
    'gesher-app: done')

# The graphics half of the NDK (docs/native-libs.md, "Graphics"; ADR 0023):
# libglapp.so (tests/apps/glapp/jni/glapp.c) draws into Gesher windows made
# with gesher_window_create, loaded by tests/native/gl_window_test.cc without
# Java. ANativeWindow_lock/unlockAndPost through libandroid's System V thunks;
# the window is read back with PrintWindow and its quadrants checked.
# EGL/GLES 3 and 2 through the generated thunks to ANGLE (libEGL.dll and
# libGLESv2.dll next to the executable): EGL_DEFAULT_DISPLAY, a window surface
# on the window's HWND, a triangle checked with glReadPixels, eglSwapBuffers.
$glTestOut = & "$Dir/gl_window_test.exe" (NoDrive "$Dir/libglapp.so") 2>&1 | Out-String
$glTestExit = $LASTEXITCODE
# OpenGL ES 3 only on a hardware Direct3D 11 device (docs/system-features.md).
# Whether this machine has one is asked of Windows directly, not of the tested
# binaries: the adapter ANGLE's hardware device takes (DXGI enumeration, as
# Renderer11 chooses) is a GPU (not flagged DXGI_ADAPTER_FLAG_SOFTWARE, not the
# Basic Render Driver 1414:008c) at Direct3D feature level 10_1 or more. The runners have no GPU,
# so there the default display gives and declares ES 2.0, has no ES 3 config
# and refuses ES 3 contexts; the ES 3 paths then run on ANGLE's own uncapped
# WARP display (test-only, through eglGetPlatformDisplayEXT). On a GPU the ES 3
# checks run on the default display.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GesherDxgiProbe {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct Desc1 {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
    public uint VendorId, DeviceId, SubSysId, Revision;
    public UIntPtr DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
    public uint LuidLow; public int LuidHigh; public uint Flags;
  }
  [DllImport("dxgi.dll")] static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr factory);
  [DllImport("d3d11.dll")] static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software,
      uint flags, IntPtr levels, uint count, uint sdk, IntPtr device, out int level, IntPtr context);
  delegate int EnumAdapters1(IntPtr self, uint index, out IntPtr adapter);
  delegate int GetDesc1(IntPtr self, out Desc1 desc);
  delegate uint Release(IntPtr self);
  static T Slot<T>(IntPtr obj, int slot) {
    return Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(obj), slot * IntPtr.Size));
  }
  // The adapter ANGLE's Direct3D 11 hardware device takes (Renderer11: the
  // first adapter not of Microsoft's vendor id, else the first adapter), as
  // "vendor:device flags level" (the Direct3D 11 feature level it gives), or
  // "none".
  public static string HardwareDeviceAdapter() {
    Guid iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");  // IDXGIFactory1
    IntPtr factory;
    if (CreateDXGIFactory1(ref iid, out factory) < 0) return "none";
    IntPtr chosen = IntPtr.Zero;
    Desc1 chosenDesc = new Desc1();
    IntPtr adapter;
    // IDXGIFactory1::EnumAdapters1 (slot 12), IDXGIAdapter1::GetDesc1 (slot 10).
    for (uint i = 0; Slot<EnumAdapters1>(factory, 12)(factory, i, out adapter) >= 0; i++) {
      Desc1 desc;
      bool described = Slot<GetDesc1>(adapter, 10)(adapter, out desc) >= 0;
      if (described && (chosen == IntPtr.Zero || (chosenDesc.VendorId == 0x1414 && desc.VendorId != 0x1414))) {
        if (chosen != IntPtr.Zero) Slot<Release>(chosen, 2)(chosen);
        chosen = adapter;
        chosenDesc = desc;
      } else {
        Slot<Release>(adapter, 2)(adapter);
      }
    }
    string result = "none";
    if (chosen != IntPtr.Zero) {
      int level;
      // D3D_DRIVER_TYPE_UNKNOWN on that adapter, the default feature levels (11_0 down to 9_1).
      if (D3D11CreateDevice(chosen, 0, IntPtr.Zero, 0, IntPtr.Zero, 0, 7, IntPtr.Zero, out level, IntPtr.Zero) < 0) level = 0;
      result = String.Format("{0:x4}:{1:x4} {2} {3:x}", chosenDesc.VendorId, chosenDesc.DeviceId, chosenDesc.Flags, level);
      Slot<Release>(chosen, 2)(chosen);
    }
    Slot<Release>(factory, 2)(factory);
    return result;
  }
}
'@
$hardwareDevice = [GesherDxgiProbe]::HardwareDeviceAdapter()
$glEs3 = $false
if ($hardwareDevice -match '^([0-9a-f]{4}):([0-9a-f]{4}) (\d+) ([0-9a-f]+)$') {
  $glEs3 = (([int]$Matches[3] -band 2) -eq 0) -and ("$($Matches[1]):$($Matches[2])" -ne '1414:008c') -and
           ([Convert]::ToInt32($Matches[4], 16) -ge 0xa100)
}
Write-Host "INFO  native libs: ANGLE's hardware device adapter $hardwareDevice (vendor:device flags feature level): OpenGL ES 3 expected $glEs3"
$es3Refused = @('es3 unavailable: no EGL_OPENGL_ES3_BIT_KHR config', 'ES 3 context refused with EGL_BAD_MATCH',
                'es3: unavailable on this device, ES 3 refused as on Android')
# The ES 3 paths on ANGLE's uncapped WARP display where the default display has no ES 3.
$es3OnAngleWarp = @('es3 (ANGLE WARP display, uncapped, test-only): EGL 1.5', 'GL_VERSION=OpenGL ES 3.',
                    'swap=ok, buffer ranges=ok', 'es3-angle-warp window read back from PrintWindow(PW_RENDERFULLCONTENT): triangle ok')
Check 'native libs: ANativeWindow_lock from an NDK .so into a Win32 window, read back with PrintWindow' {
  $global:LASTEXITCODE = $glTestExit
  $glTestOut
} @(', posted: lock ok', 'quadrants ok', 'gl_window_test all passed')

# The user's click or key in a Gesher window lets other processes take the
# foreground (the system server's picker or browser, other apps' windows):
# gesher_window.cc's WindowProc calls AllowSetForegroundWindow(ASFW_ANY) on a
# button or key going up; gl_window_test counts the calls through its import.
Check 'native libs: a click or key in a Gesher window allows other processes the foreground' {
  $global:LASTEXITCODE = $glTestExit
  $glTestOut
} @('foreground: a button or key going up in the window allows other processes the foreground (ASFW_ANY); presses and moves do not',
    'gl_window_test all passed')

if ($glEs3) {
  Check 'native libs: EGL/GLES 3 and 2 from an NDK .so through ANGLE onto a window surface; glReadPixels' {
    $global:LASTEXITCODE = $glTestExit
    $glTestOut
  } @('gesher EGL: default display: ANGLE, Direct3D 11 hardware device', 'declared OpenGL ES 3.0',
      'es3: EGL 1.5', 'es3.1 refused with EGL_BAD_MATCH', 'es2: EGL 1.5', 'GL_VERSION=OpenGL ES 3.',
      'readpixels=ok, swap=ok', 'gl_window_test all passed')
} else {
  Check 'native libs: no OpenGL ES 3 without a GPU (ES 3 refused), ES 2 from an NDK .so onto a window surface; glReadPixels' {
    $global:LASTEXITCODE = $glTestExit
    $glTestOut
  } (@('gesher EGL: default display: ANGLE, Direct3D 11', 'ES 3 configs no), declared OpenGL ES 2.0',
       'es2: EGL 1.5', 'GL_VERSION=OpenGL ES 2.', 'readpixels=ok, swap=ok', 'gl_window_test all passed') + $es3Refused)
}

# GLintptr/GLsizeiptr reach ANGLE whole (compat/bionic/gen_gl_thunks.py,
# LP64_TYPES): AOSP's khrplatform.h spells them `long`, 32 bits under mingw, so
# glBindBufferRange's size (its fifth argument, on the Microsoft stack) reached
# ANGLE with garbage above bit 31 and Flutter's Impeller drew only its clear
# colour. libglapp.so's GLES 3 run binds a range above 4 GiB and reads it back
# (glGetInteger64i_v), then draws with a uniform block bound at an aligned
# offset into a texture, blits it and checks the pixels.
if ($glEs3) {
  Check 'native libs: GLES 3 buffer ranges (64-bit GLintptr/GLsizeiptr) and a uniform block draw from an NDK .so' {
    $global:LASTEXITCODE = $glTestExit
    $glTestOut
  } @('es3: EGL 1.5', 'swap=ok, buffer ranges=ok', 'gl_window_test all passed')
} else {
  Check 'native libs: GLES 3 buffer ranges and the GLES 3 window on ANGLE''s uncapped WARP display (no GPU; test-only)' {
    $global:LASTEXITCODE = $glTestExit
    $glTestOut
  } ($es3OnAngleWarp + @('gl_window_test all passed'))
}

# What DWM composed from the swap chain, read back with PrintWindow
# (PW_RENDERFULLCONTENT): the triangle is in the window itself.
Check 'native libs: the GLES frames presented in the window, read back with PrintWindow' {
  $global:LASTEXITCODE = $glTestExit
  $glTestOut
} (@('es2 window read back from PrintWindow(PW_RENDERFULLCONTENT): triangle ok',
     'gl_window_test: GL windows read back from Windows') +
   $(if ($glEs3) { @('es3 window read back from PrintWindow(PW_RENDERFULLCONTENT): triangle ok') } else { @() }))

# The same on ANGLE's WARP device (GESHER_EGL_DEVICE=warp), capped at feature
# level 10_0: OpenGL ES 2.0 declared and given, ES 3 refused with EGL_BAD_MATCH.
$env:GESHER_EGL_DEVICE = 'warp'
$glWarpOut = & "$Dir/gl_window_test.exe" (NoDrive "$Dir/libglapp.so") 2>&1 | Out-String
$glWarpExit = $LASTEXITCODE
Remove-Item Env:GESHER_EGL_DEVICE
Check 'native libs: the same EGL/GLES window surfaces on WARP' {
  $global:LASTEXITCODE = $glWarpExit
  $glWarpOut
} (@('gesher EGL: default display: ANGLE, Direct3D 11 WARP device, feature level 10_0',
     'ES 3 configs no), declared OpenGL ES 2.0', 'GL_VERSION=OpenGL ES 2.', 'readpixels=ok, swap=ok',
     'gl_window_test: GL windows read back from Windows', 'gl_window_test all passed') + $es3Refused + $es3OnAngleWarp)

# A boot that declared ES 3.0 (ro.opengles.version, set by gl_window_test in a
# property session of its own) gives ES 3.0 on the default display even on
# WARP (uncapped then), and through the .so's eglCreateContext no ES 3.1: the
# runners, which have no GPU, check the ES 3.0 default display this way.
$env:GESHER_EGL_DEVICE = 'warp'
$glDeclaredOut = & "$Dir/gl_window_test.exe" (NoDrive "$Dir/libglapp.so") --declared 0x30000 2>&1 | Out-String
$glDeclaredExit = $LASTEXITCODE
Remove-Item Env:GESHER_EGL_DEVICE
Check 'native libs: a boot that declared ES 3.0 gives ES 3.0 to an NDK .so on WARP, and refuses ES 3.1' {
  $global:LASTEXITCODE = $glDeclaredExit
  $glDeclaredOut
} @('ro.opengles.version=196608: set', 'Direct3D 11 WARP device (boot declared OpenGL ES 3.0)',
    'es3: EGL 1.5', 'GL_VERSION=OpenGL ES 3.0', 'es3.1 refused with EGL_BAD_MATCH',
    'declared 0x30000: the default display gives 0x30000, ES 3 drawn: as declared', 'gl_window_test all passed',
    'gl-window-test: the --declared run''s data directory removed')

# AHardwareBuffer, the EGL native buffer/fence extensions and ASurfaceControl
# (ADR 0089): surface_control_test.so (tests/native/surface_control_test.c)
# checks buffers (CPU locks, ids, errors), images of buffers drawn by the GPU
# and read by the CPU and the other way round (RGBA and RGBX), native fences,
# GLES contexts current on two threads drawing in turns (ANGLE's
# angle_force_context_check_every_call), and a SurfaceControl tree whose
# transactions complete in order with their stats;
# surface_control_host_test.exe reads the composed frame back from its window
# (PrintWindow) and the empty frame once the layers leave. With a binderd of
# the test's own, a buffer also travels over an AF_UNIX socket (SCM_RIGHTS);
# without one that send fails. Then the same on WARP.
Check 'native libs: AHardwareBuffer, EGL images and native fences, and ASurfaceControl composed into a Gesher window' {
  $device = 'ahb-' + [guid]::NewGuid().ToString('N')
  $broker = Start-Process -FilePath "$Dir/binderd.exe" -ArgumentList $device -PassThru -WindowStyle Hidden
  try {
    Start-Sleep -Milliseconds 500
    $env:GESHER_BINDER = $device
    & "$Dir/surface_control_host_test.exe" (NoDrive "$Dir/surface_control_test.so")
  } finally {
    Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue
    Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
  }
} @('surface_control: ok: send/recvHandleFromUnixSocket: 0, 0',
    'surface_control: ok: what the receiver writes, the sender reads (shared memory)',
    'surface_control: ok: a CPU lock after the fence reads what the GPU drew (0 wrong)',
    'surface_control: ok: the GPU reads an RGBX buffer with alpha 1 (0 wrong)',
    'surface_control: ok: contexts current on two threads draw into their own framebuffers',
    'surface_control: ok: transactions complete in the order applied',
    'ok: the composed layers read back from the window: 0 probe(s) wrong, 0 yellow pixel(s)',
    'ok: the window after its layers left: 0 pixel(s) not black',
    $(if ($glEs3) { 'surface_control_test: EGL with ES 3 contexts' } else { 'surface_control_test: EGL with ES 2 contexts' }),
    $(if ($glEs3) { 'surface_control: ok: contexts current on two threads draw into their own framebuffers' }
      else { 'surface_control_test: ES 3 contexts on two threads on ANGLE''s uncapped WARP display (test-only)' }),
    'surface_control_test all passed')

Check 'native libs: the same AHardwareBuffer and ASurfaceControl checks on WARP, without a descriptor broker' {
  $env:GESHER_EGL_DEVICE = 'warp'
  try {
    & "$Dir/surface_control_host_test.exe" (NoDrive "$Dir/surface_control_test.so")
  } finally {
    Remove-Item Env:GESHER_EGL_DEVICE -ErrorAction SilentlyContinue
  }
} @('gesher EGL: default display: ANGLE, Direct3D 11 WARP device',
    'surface_control_test: EGL with ES 2 contexts',
    'surface_control_test: ES 3 contexts on two threads on ANGLE''s uncapped WARP display (test-only)',
    'surface_control: ok: sendHandleToUnixSocket without a descriptor broker fails',
    'ok: the composed layers read back from the window: 0 probe(s) wrong, 0 yellow pixel(s)',
    'surface_control_test all passed')

# Real pending GPU completion, independent-context CPU submission and immutable
# completed native-fence descriptor transfer. The fixture checks the private
# ANGLE synchronization ABI so an older cached SDK cannot silently pass.
Check 'native libs: asynchronous local EGL fences and native descriptor lifetime' {
  $device = 'egl-fence-' + [guid]::NewGuid().ToString('N')
  $broker = Start-Process -FilePath "$Dir/binderd.exe" -ArgumentList $device -PassThru -WindowStyle Hidden
  try {
    Start-Sleep -Milliseconds 500
    $env:GESHER_BINDER = $device
    & "$Dir/egl_fence_test.exe"
  } finally {
    Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue
    Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
  }
} @('this package contains the current ANGLE synchronization ABI',
    'ok: SCM native-fence receiver flags and ownership passed', 'egl_fence_test all passed')

Check 'native libs: the same pending EGL fence and native descriptor checks on WARP' {
  $device = 'egl-fence-warp-' + [guid]::NewGuid().ToString('N')
  $broker = Start-Process -FilePath "$Dir/binderd.exe" -ArgumentList $device -PassThru -WindowStyle Hidden
  $env:GESHER_EGL_DEVICE = 'warp'
  try {
    Start-Sleep -Milliseconds 500
    $env:GESHER_BINDER = $device
    & "$Dir/egl_fence_test.exe"
  } finally {
    Remove-Item Env:GESHER_BINDER -ErrorAction SilentlyContinue
    Stop-Process -Id $broker.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:GESHER_EGL_DEVICE -ErrorAction SilentlyContinue
  }
} @('gesher EGL: default display: ANGLE, Direct3D 11 WARP device',
    'ok: SCM native-fence receiver flags and ownership passed', 'egl_fence_test all passed')

# eglDestroyContext on one thread while another thread keeps drawing with its
# own context of the default display (issue #485,
# tests/native/egl_context_destroy_scenarios.h): two threads stepped in a
# fixed order, so thread B destroys its released context right after thread
# A's last GL call. Before compat/angle/patches/0001 thread A's next draw
# faulted in libGLESv2.dll (StateManager11's emptied draw framebuffer).
foreach ($device in 'default', 'warp') {
  Check "native libs: eglDestroyContext on one thread while another keeps drawing ($device adapter)" {
    if ($device -eq 'warp') { $env:GESHER_EGL_DEVICE = 'warp' }
    try {
      & "$Dir/egl_context_destroy_test.exe"
    } finally {
      Remove-Item Env:GESHER_EGL_DEVICE -ErrorAction SilentlyContinue
    }
  } (@('egl_context_destroy_test: ok: glUseProgram then draw: thread A''s draw after X''s destruction has its own program''s color',
      'egl_context_destroy_test: ok: draw with the same program: thread A''s draw after X''s destruction has its own program''s color',
      'egl_context_destroy_test: ok: destroying thread has another context current: thread B''s context C still draws',
      'egl_context_destroy_test all passed') +
    # The WARP run must really be on WARP, not silently on the default adapter.
    @(if ($device -eq 'warp') { 'gesher EGL: default display: ANGLE, Direct3D 11 WARP device' }))
}

# The GPU kept from sleeping between the frames Gesher reads back (child
# surfaces, SurfaceControl composition; compat/bionic/gpu_keepalive.h): on a
# hardware adapter it gives the GPU work in the gaps of frames coming 25 a
# second, none while frames come faster than every 10 ms, and stops a second
# after the last frame until the next; on a software adapter (the runners'
# Basic Render Driver, WARP) and with GESHER_GPU_KEEPALIVE=0 it stays off.
# It also checks the declared OpenGL ES version against what EGL gives: 3.0
# only on a hardware adapter; on a software one no ES 3 config, an ES 3 context
# refused with EGL_BAD_MATCH and GL_VERSION OpenGL ES 2.0.
Check 'native libs: the GPU keep-alive between frames read back (off on a software adapter)' {
  & "$Dir/gpu_keepalive_test.exe"
} @('ok: the declared version is what the configs give', 'gpu_keepalive_test all passed')

Check 'native libs: WARP declares OpenGL ES 2.0 and gives no more (no ES 3 config, ES 3 context refused)' {
  $env:GESHER_EGL_DEVICE = 'warp'
  try {
    & "$Dir/gpu_keepalive_test.exe"
  } finally {
    Remove-Item Env:GESHER_EGL_DEVICE -ErrorAction SilentlyContinue
  }
} @('ok: a software adapter: no EGL_OPENGL_ES3_BIT_KHR config',
    'ok: a software adapter declares OpenGL ES 2.0, not 3.0',
    'ok: no ES 3 configs: an ES 3 context refused with EGL_BAD_MATCH',
    'ok: no ES 3 configs: GL_VERSION OpenGL ES 2', 'gpu_keepalive_test all passed')

# An app process follows the boot's declaration (ro.opengles.version, set by
# the system server's probe), whatever its adapter has become: the test sets
# it in a property session of its own (--declared). Declared 3.0 with WARP (as
# after a Remote Desktop reconnect onto the Basic Render Driver): WARP
# uncapped, ES 3.0, no ES 3.1. Declared 2.0 (on a GPU here, or on the runners'
# Basic Render Driver): capped, ES 3 refused.
Check 'native libs: a boot that declared OpenGL ES 3.0 gives ES 3.0 on WARP, and no ES 3.1' {
  $env:GESHER_EGL_DEVICE = 'warp'
  try {
    & "$Dir/gpu_keepalive_test.exe" --declared 0x30000
  } finally {
    Remove-Item Env:GESHER_EGL_DEVICE -ErrorAction SilentlyContinue
  }
} @('ro.opengles.version=196608: set', 'ok: boot declared 3.0: this process''s display gives ES 3.0',
    'ok: an ES 3.0 display: an ES 3.1 context refused with EGL_BAD_MATCH', 'gpu_keepalive_test all passed',
    'gpu-keepalive-test: the --declared run''s data directory removed')

Check 'native libs: a boot that declared OpenGL ES 2.0 gives ES 2.0 only, on whatever device' {
  & "$Dir/gpu_keepalive_test.exe" --declared 0x20000
} @('ro.opengles.version=131072: set', 'ok: boot declared 2.0: this process''s display gives ES 2.0 only',
    'ok: no ES 3 configs: an ES 3 context refused with EGL_BAD_MATCH', 'gpu_keepalive_test all passed',
    'gpu-keepalive-test: the --declared run''s data directory removed')

Check 'native libs: GESHER_GPU_KEEPALIVE=0 turns the GPU keep-alive off' {
  $env:GESHER_GPU_KEEPALIVE = '0'
  try {
    & "$Dir/gpu_keepalive_test.exe"
  } finally {
    Remove-Item Env:GESHER_GPU_KEEPALIVE -ErrorAction SilentlyContinue
  }
} @('GESHER_GPU_KEEPALIVE=0: the keep-alive stays off', 'gpu_keepalive_test all passed')

# eglGetProcAddress and dlsym return the functions of every GL extension the
# context advertises, ANGLE's own (gl2ext_angle.h) included: WebRender calls
# glProvokingVertexANGLE for GL_ANGLE_provoking_vertex, and Firefox's GPU
# process panicked when it was null. The provoking vertex set through the
# returned pointer is read back.
Check 'native libs: the entry points of every advertised GL extension from an NDK .so' {
  & "$Dir/gl_extension_test.exe" (NoDrive "$Dir/gl_extension_procs_test.so")
} (@('GL_ANGLE_provoking_vertex: glProvokingVertexANGLE', 'is read back',
     'gl_extension_procs: all passed', 'gl_extension_test all passed') +
   $(if ($glEs3) { @('an ES 3 context') }
     else { @('on ANGLE''s WARP display, uncapped (test-only)', 'an ES 3 context on ANGLE''s uncapped WARP display') }))

# Through gesher-app: glapp.apk's Activity makes windows with gesher-app's
# NativeWindow and passes their android.view.Surface to the library, which
# takes the ANativeWindow with ANativeWindow_fromSurface; the Activity reads
# the ANativeWindow_lock window back with PrintWindow, the library checks its
# GLES 3 and 2 frames with glReadPixels (ES 3 only with a GPU; without one it
# checks that ES 3 is refused).
$glAppOut = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpNative" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/glapp.apk") 2>&1 | Out-String
$glAppExit = $LASTEXITCODE
Check 'native libs: gesher-app runs glapp.apk; ANativeWindow_fromSurface and lock into a Surface, read back' {
  $global:LASTEXITCODE = $glAppExit
  $glAppOut
} @('extracted 1 native library (x86_64)', 'GlActivity cpu: ok: window 160x96',
    'GlActivity cpu read back: ok: 160x96, 0 pixels', 'gesher-app: done')
Check 'native libs: gesher-app runs glapp.apk; EGL/GLES 3 and 2 on a Surface''s window from the .so' {
  $global:LASTEXITCODE = $glAppExit
  $glAppOut
} @($(if ($glEs3) { 'GlActivity es3: ok: EGL 1.5' }
     else { 'GlActivity es3: unavailable: EGL 1.5, es3 unavailable: no EGL_OPENGL_ES3_BIT_KHR config' }),
    'GlActivity es2: ok: EGL 1.5', 'surface 160x96', 'readpixels=ok, swap=ok', 'gesher-app: done')
