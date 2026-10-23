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

# The bionic shim: a .so built against bionic's libc, libm, libdl, liblog and
# pthread (tests/native/libc_test.c) loads through the ELF loader and passes
# its own checks: memory, strings, printf/scanf through the System V vararg
# bridge, errno, time, qsort, threads, mutexes, condition variables, keys,
# dlsym and dl_iterate_phdr.
Check 'bionic shim: a libc/pthread .so runs its checks' {
  & "$Dir/elf_load_test.exe" (NoDrive "$Dir/libc_test.so")
} @('libc_test: all passed', 'elf_load_test all passed')

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
} @('JNI_OnLoad ran: ok', 'JniTest all passed')

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
} @('NativeActivity platform: asset=Hello from an asset, assets=hello.txt, config=ok, sensors=0, looper=ok, mainlooper=armed, socketpair=ok, tcp=ok, getaddrinfo=ok, readdir=a.txt/b.txt/sub, fscanf=ok, filesystem=ok, signals=ok, strftime=ok, sysconf=ok',
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
Check 'native libs: ANativeWindow_lock from an NDK .so into a Win32 window, read back with PrintWindow' {
  $global:LASTEXITCODE = $glTestExit
  $glTestOut
} @(', posted: lock ok', 'quadrants ok', 'gl_window_test all passed')

Check 'native libs: EGL/GLES 3 and 2 from an NDK .so through ANGLE onto a window surface; glReadPixels' {
  $global:LASTEXITCODE = $glTestExit
  $glTestOut
} @('gesher EGL: default display: ANGLE, Direct3D 11', 'GL_VERSION=OpenGL ES 3.', 'GL_VERSION=OpenGL ES 2.',
    'readpixels=ok, swap=ok', 'gl_window_test all passed')

# What Windows composed from the swap chain (PrintWindow with
# PW_RENDERFULLCONTENT, or the screen under the window): reported, not gated.
($glTestOut -split "`n" | Where-Object { $_ -match 'window read back from' }) |
  ForEach-Object { Write-Host "INFO  gl_window_test: $($_.Trim())" }

# The same on ANGLE's WARP device (GESHER_EGL_DEVICE=warp).
$env:GESHER_EGL_DEVICE = 'warp'
$glWarpOut = & "$Dir/gl_window_test.exe" (NoDrive "$Dir/libglapp.so") 2>&1 | Out-String
$glWarpExit = $LASTEXITCODE
Remove-Item Env:GESHER_EGL_DEVICE
Check 'native libs: the same EGL/GLES window surfaces on WARP' {
  $global:LASTEXITCODE = $glWarpExit
  $glWarpOut
} @('gesher EGL: default display: ANGLE, Direct3D 11 WARP device', 'readpixels=ok, swap=ok',
    'gl_window_test all passed')

# Through gesher-app: glapp.apk's Activity makes windows with gesher-app's
# NativeWindow and passes their android.view.Surface to the library, which
# takes the ANativeWindow with ANativeWindow_fromSurface; the Activity reads
# the ANativeWindow_lock window back with PrintWindow, the library checks its
# GLES 3 and 2 frames with glReadPixels.
$glAppOut = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpNative" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/glapp.apk") 2>&1 | Out-String
$glAppExit = $LASTEXITCODE
Check 'native libs: gesher-app runs glapp.apk; ANativeWindow_fromSurface and lock into a Surface, read back' {
  $global:LASTEXITCODE = $glAppExit
  $glAppOut
} @('extracted 1 native library (x86_64)', 'GlActivity cpu: ok: window 96x64',
    'GlActivity cpu read back: ok: 96x64, 0 pixels', 'gesher-app: done')
Check 'native libs: gesher-app runs glapp.apk; EGL/GLES 3 and 2 on a Surface''s window from the .so' {
  $global:LASTEXITCODE = $glAppExit
  $glAppOut
} @('GlActivity es3: ok: EGL 1.5', 'GlActivity es2: ok: EGL 1.5', 'gesher-app: done')
