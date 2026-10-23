# 0015 — JNI for an app's ELF libraries: dlopen hook and System V proxies

**Status:** accepted, 2026-10-23 (native-libraries track)

## Context
`System.loadLibrary` in an app ends in ART's `JavaVMExt::LoadNativeLibrary`,
which calls libnativeloader's `OpenNativeLibrary`. On the host
(`!ART_TARGET_ANDROID`, the configuration Gesher builds) that is a plain
`dlopen(path, RTLD_NOW)`; ART then calls `dlsym` for `JNI_OnLoad` (and later
`JNI_OnUnload`) and for each native it binds by name (`Java_*`), and calls
`JNI_OnLoad` as `int (*)(JavaVM*, void*)`. On Windows `dlopen` is gesher-base's
(`compat/posix/src/dlfcn.cc`), which knew built-in libraries and PE DLLs.

The app's `.so` is System V code ([0006](0006-managed-abi.md)); the ELF loader
and the bionic shim (compat/elf, compat/bionic) load and link it. Three ABI
boundaries remain between it and ART:

1. ART calls `JNI_OnLoad`/`JNI_OnUnload` with the Microsoft ABI — the same
   call reaches libcore's built-in `JNI_OnLoad`s
   ([0011](0011-libcore-natives.md)).
2. ART enters natives with the System V ABI (`JNICALL` is `sysv_abi`), which
   suits the `.so`'s `Java_*` functions as they are.
3. ART's `JNIEnv` and `JavaVM` function tables are Microsoft ABI (they are
   implemented and called by code built by the same compiler, 0011), but the
   `.so` calls them with the System V ABI, including the variadic
   `Call*Method`/`NewObject` entries and the `va_list` forms (a System V
   `va_list` is a pointer to a 24-byte structure, a Microsoft one a `char*`).

## Decision
- **A hook in gesher-base's `dlopen`, no ART patch.** `dlopen` of a file that
  starts with the ELF magic goes to a loader registered with
  `gesher_set_elf_dlopen` (`<gesher/elf_dlopen.h>`); handles it returns are
  remembered so `dlsym`/`dlclose` on them go to the same loader. Built-in
  names and PE files behave as before. libnativeloader's host path is used
  unchanged.
- **The JNI bridge** (`compat/bionic/jni_bridge.cc`, an OBJECT library linked
  into dalvikvm by `compat/bionic/jni.cmake`) registers that loader from a
  static initializer. Its `open` installs the bionic shim on first use and
  loads the file with `elf::Load`, with the file's directory as the search
  path for `DT_NEEDED` (an APK's `libc++_shared.so` sits next to the app's
  library). Its `dlsym` returns, for
  - `JNI_OnLoad`/`JNI_OnUnload`: a generated stub (`mov r8, fn; jmp bridge`)
    that ART calls with the Microsoft ABI; the bridge records the real
    `JavaVM`, sets the thread up and calls the `.so`'s System V function with
    the proxy `JavaVM`;
  - `Java_*`: a generated stub (`mov r11, fn; jmp entry`) to an assembly entry
    that saves the System V argument registers, calls `gesher_jni_enter`
    (which runs `elf::SetupThreadTls()` once per thread, [0014](0014-native-libs-fs-base.md),
    and maps ART's `JNIEnv*` to the thread's proxy), restores them and jumps
    to the `.so`'s function, so stack arguments and the return go straight
    between ART and the `.so`. Functions the `.so` registers with
    `RegisterNatives` get the same stub. The `JNIEnv*` is replaced only when it
    is this thread's (`GetEnv`), so a `@CriticalNative` method's first
    argument passes unchanged;
  - anything else: the address itself.
- **Proxies with System V tables.** The `.so` never sees ART's `JNIEnv`: each
  thread has a proxy `{functions, real}` whose table holds System V functions
  that call the same entry of the real `JNIEnv`'s table (a template generates
  the ~200 plain ones from the `JNINativeInterface` member types). The proxy
  `JavaVM`'s `GetEnv`, `AttachCurrentThread(AsDaemon)` hand out the calling
  thread's proxy; `GetJavaVM` gives the proxy `JavaVM`; `RegisterNatives`
  wraps each function in an entry stub. The shim also exports
  `JNI_GetCreatedJavaVMs` (System V) with the proxy.
- **Variadic and `va_list` calls by parameter types.** The 31 variadic entries
  are assembly trampolines (the shim's `GESHER_VA_TRAMPOLINE`) that build a
  System V `va_list` and call the `va_list` forms; those take the `.so`'s own
  `va_list` too (C++'s `JNIEnv` wrappers use them). They read the arguments by
  the method's parameter types into a `jvalue` array and call ART's
  `Call*MethodA`/`NewObjectA`. The types come from the signature given to
  `GetMethodID`/`GetStaticMethodID` (recorded per `jmethodID`), or, for an id
  from elsewhere (`FromReflectedMethod`), from
  `Executable.getParameterTypes()` once.
- **gesher-app** copies an APK's `lib/x86_64/*.so` into
  `ApplicationInfo.nativeLibraryDir` before the launch
  (`app/java/com/gesher/app/NativeLibs.java`), as the package manager does for
  `extractNativeLibs`; the app's `PathClassLoader` already searches that
  directory, so `System.loadLibrary` resolves to it.

## Consequences
- No ART or libnativeloader patch; the only edit outside the track's files is
  the hook in gesher-base's `dlfcn.cc`.
- A native call costs an extra stub, register save/restore and a
  thread-local check; each `JNIEnv` call from the `.so` an extra forward;
  variadic calls build a `jvalue` array.
- ART's `dlsym` of a non-JNI symbol in an ELF handle returns the raw System V
  address: Windows code must not call it directly.
- A `.so` that keeps a `JNIEnv*` across threads, or hands the proxy to another
  thread, gets that thread's real `JNIEnv` behind it (as wrong as on Android).
- JVMTI environments from `GetEnv` are passed through unwrapped (Microsoft
  ABI tables): agents in an app's `.so` are not supported.
- Loaded libraries are not linker-namespaced: every app library and its
  dependencies share one global ELF scope.
