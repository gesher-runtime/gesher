# Native library namespaces, search and identity

N02 and N03 in [issue #56](https://github.com/gesher-runtime/gesher/issues/56)
build on [atomic loading](native-load-transactions.md) and
[symbol versions](native-symbol-versions.md). These contracts apply to ordinary
applications, JNI and NativeActivity, without package-specific exceptions.

The reference is Android 16 r4: Bionic
`f22516cbc67e81c13cf943ce02f01b8929a98726`, ART
`1690c6912a7972c9e62c39b48c706de9b8b18b4a` and frameworks/base
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4`. Primary sources include the
[linker][linker], [namespace membership][namespaces], [libdl wrappers][libdl],
[ART namespace policy][art-namespaces], [JNI ownership][art-jni] and
[package dependency policy][package-policy].

## Context and classloader ownership

`LoaderContext` carries a namespace, actual caller address and guest
architecture. A namespace owns ordered search paths, permitted roots, target SDK,
membership and named links. Context belongs to an operation; there is no mutable
process-wide “current classloader.”

ART's target-specific Gesher host branch forwards the existing
`CreateClassLoaderNamespace` and `OpenNativeLibrary` arguments to
`compat/art/native_loader.cc`. ClassLoaderFactory and NativeActivity share
that adapter. This does not globally enable `ART_TARGET_ANDROID`.

The actual `VMRuntime.setTargetSdkVersion` initializer forwards the application's
SDK to the common loader's native getter. Creating a classloader with its own
target policy does not overwrite that application value. Zero follows Bionic's
meaning of the shipped platform API.
Linker compatibility uses the application SDK when a new local group is loaded;
an existing group's snapshot is retained. The classloader SDK continues to
control namespace construction and native-library declaration policy.

The registry uses weak JNI identities and `IsSameObject`, finds the nearest
registered parent, and preserves isolated/shared creation. An explicitly empty
native list differs from null for an unknown custom classloader: pinned ART
permits the unknown loader's public libraries. Neither grants private providers.

The versioned `gesher_elf_namespace_ops` extension preserves the original
three-field ELF open/symbol/close hook. Scoped opens return handles recognized by
ordinary symbol/close dispatch. Boot host builtins and deliberate PE loads use
the separate host route; an app's path cannot acquire an unrelated builtin
through a basename fallback.

Scoped symbol lookup and the direct/global version wrappers expose the same
three-state result: absent, resolved (including a null IFUNC), or selected TLS
with unavailable current-thread storage. The shim uses that result without
repeating resolution or searching another provider after a selected definition.
Namespace, caller and architecture selection remain part of the same search.

Namespace configuration acquires `ScopedLoaderOperation` before the JNI
registry mutex. Parent Java calls and catalog I/O precede both locks. Ordinary
registry lookups release their mutex before loading. No registry mutex crosses
constructors/JNI callbacks. This keeps N01 publication atomic and avoids the
inverse registry-to-loader lock order during concurrent constructor reentry.

ART destroys its Runtime and owned JavaVM before calling `ResetNativeLoader`.
The adapter follows [ART's metadata-only reset][art-reset]: it drops native weak
identity records without calling JNI through a retired VM. The VM owns disposal
of its reference table. The native registry is cleared under the existing
loader/registry lock order. Bionic's process anonymous namespace, other namespace
identities and live ELF resources remain retained. The pinned linker's
`set_anonymous_namespace` and `create_namespace` reject a second anonymous
assignment; only its explicitly test-only `init_anonymous_namespace` resets that
flag. ART's registry Reset does not provide general VM recreation, replacement
of the process anonymous namespace or successful ELF unloading.

Persistent public-file domains reuse identical ordered namespace links. Repeated
classloader creation therefore does not append another copy of the same link
and SONAME vector or extend every unsuccessful search. Deduplication preserves
the first edge's position; a different destination or ordered list retains the
existing append behavior. It does not change a library's visibility policy.

## Identity is distinct from a name

| Request | Reuse rule |
| --- | --- |
| Bare name | Consider an accessible loaded SONAME in the applicable namespace. |
| Explicit path | Open that file; a matching basename cannot choose a different file. |
| File alias | Compare retained-descriptor device, inode and offset within namespace visibility. |
| Independent isolated namespace | The same file can have a separate mapping and constructor state. |
| Allowed namespace link | Reuse the visible instance according to the link. |
| Memory image | An explicit stable source token permits reuse; a display name alone does not. |

Canonical paths for access checks and `ORIGIN` come from the opened file.
SONAME comparison preserves case. A missing SONAME stays absent for target SDK
23 and later; the basename fallback is limited to the older contract.
Private load graphs and published objects use the same identity rules.
Reused aliases share storage and do not repeat ELF constructors.

ART separately tracks JNI ownership and `JNI_OnLoad` results by path. The
pinned implementation leaves inode canonicalization as future work. ELF alias
reuse therefore does not invent alias-wide JNI_OnLoad deduplication or change
ART's rejection of an already owned JNI path in another classloader.

## Search and symbol groups

Dependency search orders namespace library-path overrides, the requester's
`DT_RUNPATH`, namespace defaults and permitted links. Expand
`$ORIGIN`/`${ORIGIN}` and `$LIB`/`${LIB}` for that requester.
A root RUNPATH is not inherited by its children. Loading an unrelated library
does not replace a global app search path.

Relocations use the global group and local breadth-first dependency group,
retaining `DF_1_GLOBAL`, `RTLD_GLOBAL`, target-SDK local visibility,
`DT_SYMBOLIC`, protected/local binding and N05 version matching.

| Lookup | Scope |
| --- | --- |
| Explicit handle | The actual library/provider and accessible dependencies. |
| `RTLD_DEFAULT` | The actual caller's namespace and applicable global/local groups. |
| `RTLD_NEXT` | That group after an identified caller; unknown caller is an error. |
| Main handle | The main/default namespace's actual scope. |

Public x86 libdl wrappers capture the real return PC. Translated ARM wrappers
carry guest X30 saved by the translator, with nested restoration. A host
trampoline PC does not stand in for a guest caller. ARM representation overrides
follow actual scoped selection, preserving an ELF definition with a shim name.

The Android LP64 `RTLD_*` mask is validated. Eager relocation serves lazy
and immediate requests; visibility and `NOLOAD` are not ignored. NOLOAD
cannot map a missing object or run its constructor. Complete reference-counted
unloading remains N06.

## Providers and native declarations

A provider describes an actually supplied library: SONAME, explicit aliases,
ABI availability, visibility, owned exports and legitimate dependency edges.
It does not fabricate ELF mappings, program headers or version definitions.
Handles can reach their real dependencies, but cannot fall through to the
aggregate Bionic table.

The libc/libm/libdl, log, compression, native window and graphics registrations
have explicit ownership. GL ownership follows pinned AOSP exports: GLES1 uses
its version-script map; EGL and GLES2/3 also include public definitions from
their implementation sources when the shipped wrapper exists.
Proc-address extensions do not automatically become every GL library's dlsym
exports. The aggregate `LookupBionicSymbol` remains internal plumbing.
The implemented `JNI_GetCreatedJavaVMs` entry belongs to NDK-public
`libnativehelper.so` as well as private ART, following the [API 31 export map][nativehelper-map].
The native-loader initializer registers the actual JNI bridge before the first
classloader or catalog snapshot. Registration does not create a VM, enter guest
code or allocate guest TLS. Other unimplemented invocation exports remain absent.
An embedder can explicitly enable its own unscoped resolver; ordinary JNI/guest
loads cannot. Missing required dependencies and unresolved strong imports fail
loading. Diagnostic function/data placeholders are disabled by default.

`nativeBuiltinCatalog` and `nativePublicFileCatalog` copy SONAME,
classification and ABI rows from available providers and installed public-library
configuration backed by real ELF files. PackageManager and the namespace adapter
consume the same availability policy. Missing files and wrong ABIs cannot become
advertised device libraries.

NDK-public libraries remain automatically accessible. Device-public additions
follow `uses-native-library` and the pinned SDK/compatibility change.
Private libraries are excluded. The installer validates the selected ABI for
the complete APK set before allocating a UID/generation or replacing state.
Missing required libraries report `INSTALL_FAILED_MISSING_SHARED_LIBRARY`;
optional absence permits installation and leaves ordinary load failure intact.
Long-lived package metadata revalidates the catalog.

Native `SharedLibraryInfo` entries are merged after the Java shared-library
graph. They convey native names to LoadedApk/ClassLoaderFactory without adding
ELF paths to `sharedLibraryFiles` or the Java dex context. NDK automatic
visibility is separate from PackageManager's device-library declarations.
See the [manifest contract][manifest] for required/optional behavior.

## Validation boundaries

Fixtures are original valid C/ELF and compiler-produced APK/DEX files.
`build-elf-namespaces.sh` creates actual SONAME/RUNPATH/NEEDED/visibility
combinations and real guest call sites. Private volatile constructor storage
keeps compilers from folding the constructor observation into a constant.

| Gate | Evidence and boundary |
| --- | --- |
| `tools/test_elf_namespaces.py` | Common loader with valid DSOs; Linux uses explicitly registered test callbacks. |
| `tools/test_guest_libdl_namespaces.py` | Production libdl implementation and guest calls; its Linux extraction boundary is identified. |
| `guest_libdl_namespaces_test` | Complete Windows Bionic/ELF linkage; `--arm64` uses the shipped translator. |
| `tools/test_gl_provider_ownership.py` | Actual pinned GL headers, maps and implementation definitions against generated ownership metadata; not GL backend execution. |
| `tools/test_bionic_provider_ownership.py` | Actual Registry ownership against pinned Bionic maps generated separately for x86-64 and ARM64; explicit inert function-address seams do not test implementation behavior. |
| `tools/test_native_namespaces.py` | Real host JVM JNI identities with the production registry/adapter; not ART execution. |
| `tools/test_native_activity_dispatch.py` | The production NativeActivity entry/callback adapter with actual x86 or translated ARM64 ELF code; not the Windows ART/UI lifecycle. |
| `native_activity_guest_test` | The same entry/callback adapter linked against the complete Windows shim, with real NDK fixtures for both ISAs; not the ART/UI lifecycle. |
| `NativeNamespaceArtTest` | Shipped ART ClassLoaderFactory, first-load versioned nativehelper import and VM identity, System.load, JNI_OnLoad and namespace visibility. |
| `NativeLibraryInstallTest` | Real APK manifests, PackageParser2 and Installer rejection; old-state fixture setup is separate from the production update. |

Cross-compilation/linking proves platform declarations and link completeness,
not Windows runtime success. An Android-targeted DSO or API36 android.jar is not
an Android reference execution. Record exact source/fixture hashes, commands,
timing and resource measurements for each acceptance run.

The real-JVM host fixture inspects the actual production namespace link vectors:
1,000 identical registrations retain one entry and unchanged capacity, distinct
grants retain their order, and 300 transient classloaders retain the same shared
file-domain link count. After `DestroyJavaVM` succeeds, it calls the production
Reset without further JNI access, verifies retained process anonymous selection
and refusal of a second assignment, and checks repeated reset. These are HotSpot/native metadata checks,
not execution of the shipped ART teardown path on Windows.

This change does not absorb N04 fd/offset/reserved-address/RELRO, N06 complete
unloading, N07 direct APK ZIP loading or N08 dynamic ARM TLS. Namespace/provider
identities remain stable for the process lifetime. Retiring a weak JNI reference
does not claim that live ELF resources were unloaded. The native Windows ARM
thread-pointer limit in [N05](native-symbol-versions.md) remains explicit.

[linker]: https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker.cpp
[namespaces]: https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/linker/linker_namespaces.h
[libdl]: https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libdl/libdl.cpp
[art-namespaces]: https://android.googlesource.com/platform/art/+/1690c6912a7972c9e62c39b48c706de9b8b18b4a/libnativeloader/library_namespaces.cpp
[art-jni]: https://android.googlesource.com/platform/art/+/1690c6912a7972c9e62c39b48c706de9b8b18b4a/runtime/jni/java_vm_ext.cc
[art-reset]: https://android.googlesource.com/platform/art/+/1690c6912a7972c9e62c39b48c706de9b8b18b4a/libnativeloader/library_namespaces.h
[nativehelper-map]: https://android.googlesource.com/platform/libnativehelper/+/2d584ec2da2c5271ed1576593c6590d652806f0d/libnativehelper.map.txt
[package-policy]: https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/pm/SharedLibrariesImpl.java
[manifest]: https://developer.android.com/guide/topics/manifest/uses-native-library-element
